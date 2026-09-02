//===- Driver.cpp - weavec-cc, the compiler driver ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Driver.h"

#include "weavec/Config/Version.h"
#include "weavec/Frontend/ProgramAnalysis.h"
#include "weavec/Frontend/ResourceDir.h"
#include "weavec/Frontend/Sidecar.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Driver/Action.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Driver/Types.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/FrontendTool/Utils.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace weavec::frontend {

//===----------------------------------------------------------------------===//
// Flags
//===----------------------------------------------------------------------===//

bool DriverOptions::consume(llvm::StringRef arg, std::string &error) {
  bool value = true;
  llvm::StringRef name = arg;
  if (name.consume_front("-fno-")) {
    value = false;
  } else if (!name.consume_front("-f")) {
    if (control.parse(arg, error)) {
      spellings.push_back(arg.str());
      return true;
    }
    return false;
  }

  struct Flag {
    llvm::StringLiteral name;
    bool DriverOptions::*member;
  };
  static constexpr std::array<Flag, 6> Flags{{
      {.name = "weavec", .member = &DriverOptions::enabled},
      {.name = "weavec-strict", .member = &DriverOptions::strict},
      {.name = "weavec-report-unannotated",
       .member = &DriverOptions::reportUnannotated},
      {.name = "weavec-analyze-headers",
       .member = &DriverOptions::analyzeHeaders},
      {.name = "weavec-dump-analysis", .member = &DriverOptions::dumpAnalysis},
      {.name = "weavec-link", .member = &DriverOptions::link},
  }};
  const auto *const found =
      llvm::find_if(Flags, [name](const Flag &f) { return name == f.name; });
  if (found == Flags.end()) {
    if (!name.starts_with("weavec"))
      return false;
    error = "unknown WeaveC flag '" + arg.str() + "'";
    return true;
  }
  this->*found->member = value;
  spellings.push_back(arg.str());
  return true;
}

FrontendOptions DriverOptions::toFrontendOptions() const {
  FrontendOptions options;
  options.analysis.strictExterns = strict;
  options.analysis.reportUnannotated = reportUnannotated;
  options.analysis.dumpStream = dumpAnalysis ? &llvm::outs() : nullptr;
  options.mainFileOnly = !analyzeHeaders;
  options.control = control;
  return options;
}

//===----------------------------------------------------------------------===//
// The -cc1 step
//===----------------------------------------------------------------------===//

namespace {

/// Clang's frontend action for the job (code generation, usually) with
/// WeaveC's consumer running first over the same AST, so a WeaveC error
/// stops the object from being written like any other error would.
class WeaveCWrapperAction final : public clang::WrapperFrontendAction {
public:
  WeaveCWrapperAction(std::unique_ptr<clang::FrontendAction> wrapped,
                      FrontendOptions opts)
      : WrapperFrontendAction(std::move(wrapped)), options(std::move(opts)) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override {
    std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
    consumers.push_back(createWeaveCConsumer(compiler, options));
    if (auto inner = WrapperFrontendAction::CreateASTConsumer(compiler, inFile))
      consumers.push_back(std::move(inner));
    return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
  }

private:
  FrontendOptions options;
};

} // namespace

static void initializeTargets() {
  static const bool Initialized = [] {
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();
    return true;
  }();
  (void)Initialized;
}

/// Best-effort removal of a file that may not exist.
static void removeQuietly(llvm::StringRef path) {
  std::ignore = llvm::sys::fs::remove(path, /*IgnoreNonExisting=*/true);
}

static std::string currentDirectory() {
  llvm::SmallString<256> cwd;
  if (llvm::sys::fs::current_path(cwd))
    return {};
  return cwd.str().str();
}

int runCc1(llvm::ArrayRef<const char *> argv, const char *argv0) {
  DriverOptions weavec;
  std::vector<const char *> cc1Args;
  for (const char *arg : argv) {
    std::string error;
    if (weavec.consume(arg, error)) {
      if (!error.empty()) {
        llvm::errs() << "weavec-cc: error: " << error << '\n';
        return 1;
      }
      continue;
    }
    cc1Args.push_back(arg);
  }

  initializeTargets();

  auto compiler = std::make_unique<clang::CompilerInstance>();
  auto diagIds = llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>();

  // Diagnostics from parsing the command line are buffered until the real
  // engine, configured by that command line, exists (as cc1_main does).
  clang::DiagnosticOptions parseDiagOptions;
  auto *buffer = new clang::TextDiagnosticBuffer;
  clang::DiagnosticsEngine parseDiags(diagIds, parseDiagOptions, buffer);
  bool success = clang::CompilerInvocation::CreateFromArgs(
      compiler->getInvocation(), cc1Args, parseDiags, argv0);

  // The driver passes -resource-dir; a hand-written cc1 line may not. Clang
  // would guess relative to this executable, which is wrong for us.
  clang::HeaderSearchOptions &headers = compiler->getHeaderSearchOpts();
  if (headers.UseBuiltinIncludes && headers.ResourceDir.empty())
    headers.ResourceDir = getClangResourceDir();

  compiler->createDiagnostics();
  if (!compiler->hasDiagnostics())
    return 1;
  buffer->FlushDiagnostics(compiler->getDiagnostics());
  if (!success)
    return 1;

  if (!compiler->getFrontendOpts().LLVMArgs.empty()) {
    std::vector<const char *> args{"weavec-cc (LLVM option parsing)"};
    for (const std::string &arg : compiler->getFrontendOpts().LLVMArgs)
      args.push_back(arg.c_str());
    llvm::cl::ParseCommandLineOptions(static_cast<int>(args.size()),
                                      args.data());
  }

  // WeaveC checks C. Anything else, and anything that never builds an AST,
  // is Clang's alone.
  const clang::LangOptions &lang = compiler->getLangOpts();
  const bool analyse = weavec.enabled && !lang.CPlusPlus && !lang.ObjC;
  if (!analyse)
    return clang::ExecuteCompilerInvocation(compiler.get()) ? 0 : 1;

  std::unique_ptr<clang::FrontendAction> inner =
      clang::CreateFrontendAction(*compiler);
  if (!inner)
    return 1;
  if (inner->usesPreprocessorOnly())
    return compiler->ExecuteAction(*inner) ? 0 : 1;

  // The compile step sees the unit alone; boundaries wait for the link.
  FrontendOptions options = weavec.toFrontendOptions();
  options.analysis.deferBoundary = true;
  std::optional<UnitResult> result;
  options.onResult = [&result](UnitResult r) { result = std::move(r); };

  WeaveCWrapperAction action(std::move(inner), std::move(options));
  success = compiler->ExecuteAction(action);

  const std::string &output = compiler->getFrontendOpts().OutputFile;
  if (!output.empty() && output != "-") {
    const std::string sidecar = sidecarPathFor(output);
    if (success && result) {
      UnitRecord record;
      record.exports = std::move(result->exports);
      record.reported = std::move(result->reported);
      record.workingDirectory = currentDirectory();
      for (const char *arg : cc1Args)
        record.command.emplace_back(arg);
      std::string error;
      if (!writeSidecar(sidecar, record, &error))
        llvm::errs() << "weavec-cc: warning: " << error << '\n';
    } else {
      removeQuietly(sidecar);
    }
  }
  return success ? 0 : 1;
}

//===----------------------------------------------------------------------===//
// The link step
//===----------------------------------------------------------------------===//

namespace {

/// A unit re-parsed from the cc1 command line its sidecar recorded.
class Cc1Unit final : public ProgramUnit {
public:
  Cc1Unit(std::string displayName, std::vector<std::string> command,
          std::string workingDirectory, const char *argv0)
      : display(std::move(displayName)), args(std::move(command)),
        cwd(std::move(workingDirectory)), argv0(argv0) {}

  [[nodiscard]] std::string name() const override { return display; }

  bool run(clang::tooling::FrontendActionFactory &factory) override {
    std::vector<const char *> argv;
    argv.reserve(args.size());
    for (const std::string &arg : args)
      argv.push_back(arg.c_str());

    auto invocation = std::make_shared<clang::CompilerInvocation>();
    auto diagIds = llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>();
    clang::DiagnosticOptions parseDiagOptions;
    clang::DiagnosticsEngine parseDiags(
        diagIds, parseDiagOptions,
        new clang::TextDiagnosticPrinter(llvm::errs(), parseDiagOptions));
    if (!clang::CompilerInvocation::CreateFromArgs(*invocation, argv,
                                                   parseDiags, argv0)) {
      llvm::errs() << "weavec-cc: error: the recorded command for '" << display
                   << "' no longer runs\n";
      return false;
    }
    if (!cwd.empty())
      invocation->getFileSystemOpts().WorkingDir = cwd;
    // Only the analysis runs; nothing is written.
    invocation->getFrontendOpts().OutputFile.clear();
    if (invocation->getHeaderSearchOpts().ResourceDir.empty())
      invocation->getHeaderSearchOpts().ResourceDir = getClangResourceDir();

    clang::CompilerInstance compiler(std::move(invocation));
    compiler.createDiagnostics();
    if (!compiler.hasDiagnostics())
      return false;
    const std::unique_ptr<clang::FrontendAction> action = factory.create();
    return compiler.ExecuteAction(*action);
  }

private:
  std::string display;
  std::vector<std::string> args;
  std::string cwd;
  const char *argv0;
};

struct LinkInput {
  std::string object;
  UnitRecord record;
};

} // namespace

static bool newerThan(llvm::StringRef a, llvm::StringRef b) {
  llvm::sys::fs::file_status statusA;
  llvm::sys::fs::file_status statusB;
  if (llvm::sys::fs::status(a, statusA) || llvm::sys::fs::status(b, statusB))
    return false;
  return statusA.getLastModificationTime() > statusB.getLastModificationTime();
}

/// Reads the sidecar of every object on the link line.
static std::vector<LinkInput>
collectLinkInputs(const clang::driver::Command &link) {
  std::vector<LinkInput> inputs;
  for (const clang::driver::InputInfo &input : link.getInputInfos()) {
    if (!input.isFilename())
      continue;
    const std::string object = input.getFilename();
    const std::string path = sidecarPathFor(object);
    if (!llvm::sys::fs::exists(path))
      continue;
    if (newerThan(object, path)) {
      llvm::errs() << "weavec-cc: warning: ignoring '" << path
                   << "': older than '" << object << "'\n";
      continue;
    }
    std::string error;
    std::optional<UnitRecord> record = readSidecar(path, &error);
    if (!record) {
      llvm::errs() << "weavec-cc: warning: ignoring '" << path << "': " << error
                   << '\n';
      continue;
    }
    inputs.push_back(LinkInput{.object = object, .record = std::move(*record)});
  }
  return inputs;
}

/// The whole-program step: true if the link may proceed.
static bool runLinkStep(const clang::driver::Command &link,
                        const DriverOptions &weavec, const char *argv0) {
  std::vector<LinkInput> inputs = collectLinkInputs(link);
  if (inputs.empty())
    return true;

  // Which units another unit's definitions or callbacks can affect.
  std::map<std::string, std::vector<unsigned>, std::less<>> candidates;
  std::set<std::string> defined;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    for (const auto &[name, function] : inputs[i].record.exports.functions) {
      if (function.external)
        defined.insert(name);
      if (function.addressTaken && !function.typeKey.empty())
        candidates[function.typeKey].push_back(i);
    }
  }

  ProgramAnalysis program(weavec.toFrontendOptions());
  for (unsigned i = 0; i < inputs.size(); ++i) {
    UnitRecord &record = inputs[i].record;
    const analysis::UnitExports &exports = record.exports;
    bool needsAnalysis = !exports.unknownCallees.empty() ||
                         !exports.unknownIndirectTypes.empty();
    needsAnalysis = needsAnalysis ||
                    llvm::any_of(exports.imports, [&](const std::string &name) {
                      return defined.contains(name);
                    });
    needsAnalysis =
        needsAnalysis ||
        llvm::any_of(exports.indirectTypes, [&](const std::string &key) {
          const auto it = candidates.find(key);
          return it != candidates.end() &&
                 llvm::any_of(it->second, [i](unsigned u) { return u != i; });
        });

    const std::string name =
        exports.source.empty() ? inputs[i].object : exports.source;
    if (needsAnalysis && !record.command.empty()) {
      program.addUnit(
          std::make_unique<Cc1Unit>(name, std::move(record.command),
                                    std::move(record.workingDirectory), argv0),
          record.exports, std::move(record.reported));
    } else {
      program.addExports(record.exports);
    }
  }

  const ProgramAnalysis::Result result = program.run();
  for (const std::string &name : result.failed)
    llvm::errs() << "weavec-cc: error: cannot re-analyse '" << name << "'\n";
  for (const std::vector<std::string> &component : result.nonConverging) {
    llvm::errs() << "weavec-cc: error: whole-program analysis of ";
    llvm::interleaveComma(component, llvm::errs(), [](const std::string &n) {
      llvm::errs() << '\'' << n << '\'';
    });
    llvm::errs() << " did not converge\n";
  }
  return result.ok();
}

//===----------------------------------------------------------------------===//
// The driver
//===----------------------------------------------------------------------===//

static bool looksLikeSource(llvm::StringRef arg) {
  if (arg.starts_with("-"))
    return false;
  const llvm::StringRef ext = llvm::sys::path::extension(arg);
  if (ext.empty())
    return false;
  const clang::driver::types::ID type =
      clang::driver::types::lookupTypeForExtension(ext.drop_front());
  return type != clang::driver::types::TY_INVALID &&
         clang::driver::types::isSrcFile(type);
}

static int delegateToClang(llvm::ArrayRef<const char *> argv) {
  const std::string clang = getClangExecutable();
  if (clang.empty()) {
    llvm::errs() << "weavec-cc: error: cannot find a clang binary to run '"
                 << argv[1] << "' (set WEAVEC_CLANG)\n";
    return 1;
  }
  std::vector<llvm::StringRef> args;
  args.emplace_back(clang);
  for (const char *arg : argv.drop_front())
    args.emplace_back(arg);
  std::string error;
  const int rc =
      llvm::sys::ExecuteAndWait(clang, args, std::nullopt, {}, 0, 0, &error);
  if (!error.empty())
    llvm::errs() << "weavec-cc: error: " << error << '\n';
  return rc;
}

static void printVersion(llvm::raw_ostream &os) {
  os << "weavec-cc version " << WEAVEC_VERSION_STRING << " ("
     << WEAVEC_GIT_REVISION;
  if (WEAVEC_GIT_DIRTY)
    os << "-dirty";
  os << ")\n  built with LLVM " << WEAVEC_LLVM_VERSION_STRING << "\n";
}

// The in-process cc1 entry point Clang's driver calls; `argv` is
// `<executable> -cc1 <args...>`.
static int cc1Entry(llvm::SmallVectorImpl<const char *> &argv) {
  return runCc1(llvm::ArrayRef(argv).drop_front(2), argv[0]);
}

int runDriver(llvm::ArrayRef<const char *> argv, void *mainAddress) {
  if (argv.size() > 1) {
    const llvm::StringRef mode = argv[1];
    if (mode == "-cc1")
      return runCc1(argv.drop_front(2), argv[0]);
    if (mode.starts_with("-cc1"))
      return delegateToClang(argv);
  }

  const std::string executable =
      llvm::sys::fs::getMainExecutable(argv[0], mainAddress);

  DriverOptions weavec;
  std::vector<const char *> clangArgs;
  // Storage for arguments synthesised here; a deque keeps the pointers
  // handed to `clangArgs` valid as it grows.
  std::deque<std::string> owned;
  bool printJobsOnly = false;
  bool hasSysroot = false;
  bool hasSource = false;
  clangArgs.push_back(argv[0]);
  for (const char *arg : argv.drop_front()) {
    std::string error;
    if (weavec.consume(arg, error)) {
      if (!error.empty()) {
        llvm::errs() << "weavec-cc: error: " << error << '\n';
        return 1;
      }
      continue;
    }
    const llvm::StringRef text(arg);
    if (text == "--version")
      printVersion(llvm::outs());
    if (text == "-###")
      printJobsOnly = true;
    if (text.starts_with("-isysroot") || text.starts_with("--sysroot"))
      hasSysroot = true;
    if (looksLikeSource(text))
      hasSource = true;
    clangArgs.push_back(arg);
  }

  const auto add = [&](std::string arg) {
    owned.push_back(std::move(arg));
    clangArgs.push_back(owned.back().c_str());
  };
  if (hasSource) {
    // Every cc1 job gets WeaveC's flags back, `-fno-weavec` included.
    for (const std::string &flag : weavec.spellings) {
      add("-Xclang");
      add(flag);
    }
    if (weavec.enabled) {
      const std::string include = findResourceIncludeDir(argv[0], mainAddress);
      if (!include.empty()) {
        add("-isystem");
        add(include);
      }
      add("-D__WEAVEC__=1");
    }
  }
  if (!hasSysroot) {
    const std::string sysroot = getDefaultSysroot();
    if (!sysroot.empty()) {
      add("-isysroot");
      add(sysroot);
    }
  }

  clang::DiagnosticOptions diagOptions;
  auto *printer = new clang::TextDiagnosticPrinter(llvm::errs(), diagOptions);
  printer->setPrefix("weavec-cc");
  clang::DiagnosticsEngine diags(
      llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), diagOptions, printer);

  clang::driver::Driver driver(executable, llvm::sys::getDefaultTargetTriple(),
                               diags, "weavec-cc: WeaveC C compiler");
  driver.setTargetAndMode(
      clang::driver::ToolChain::getTargetAndModeFromProgramName("clang"));
  driver.CC1Main = cc1Entry;
  if (const std::string resources = getClangResourceDir(); !resources.empty())
    driver.ResourceDir = resources;

  initializeTargets();
  const std::unique_ptr<clang::driver::Compilation> compilation(
      driver.BuildCompilation(clangArgs));
  if (!compilation || diags.hasErrorOccurred())
    return 1;
  if (printJobsOnly) {
    compilation->getJobs().Print(llvm::errs(), "\n", /*Quote=*/true);
    return 0;
  }

  // Every cc1 job runs here, whatever their number, so that a WeaveC flag,
  // a sidecar and a diagnostic mean the same thing in a one-step build as
  // in a `-c` build.
  for (clang::driver::Command &job : compilation->getJobs()) {
    if (!job.getArguments().empty() &&
        llvm::StringRef(job.getArguments().front()) == "-cc1")
      job.InProcess = true;
  }

  int status = 0;
  for (const clang::driver::Command &job : compilation->getJobs()) {
    const bool isLink =
        job.getSource().getKind() == clang::driver::Action::LinkJobClass;
    if (isLink && weavec.enabled && weavec.link &&
        !runLinkStep(job, weavec, executable.c_str())) {
      status = 1;
      break;
    }
    const clang::driver::Command *failing = nullptr;
    const int rc = compilation->ExecuteCommand(job, failing);
    if (rc != 0) {
      status = rc < 0 ? 1 : rc;
      if (failing != nullptr) {
        const auto *action =
            llvm::dyn_cast<clang::driver::JobAction>(&failing->getSource());
        compilation->CleanupFileMap(compilation->getResultFiles(), action);
        compilation->CleanupFileMap(compilation->getFailureResultFiles(),
                                    action);
      }
      break;
    }
  }

  // Temporary objects vanish with the compilation; so should their sidecars.
  for (const char *temp : compilation->getTempFiles())
    removeQuietly(sidecarPathFor(temp));
  return status;
}

} // namespace weavec::frontend
