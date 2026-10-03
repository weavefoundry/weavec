//===- Driver.cpp - weavec-cc, the compiler driver ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Driver.h"

#include "weavec/Config/Version.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/CheckEmitter.h"
#include "weavec/Frontend/ClangDiagnosticSink.h"
#include "weavec/Frontend/DeferredCodeGenConsumer.h"
#include "weavec/Frontend/DispatchEdges.h"
#include "weavec/Frontend/LedgerOutput.h"
#include "weavec/Frontend/LinkStep.h"
#include "weavec/Frontend/ObjectRegistration.h"
#include "weavec/Frontend/ProgramAnalysis.h"
#include "weavec/Frontend/RecordFacts.h"
#include "weavec/Frontend/RecordPayload.h"
#include "weavec/Frontend/ResourceDir.h"
#include "weavec/Frontend/UnitRecord.h"
#include "weavec/Frontend/ZeroInit.h"

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
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Options/Options.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace weavec::frontend {

//===----------------------------------------------------------------------===//
// Flags
//===----------------------------------------------------------------------===//

/// `invalid value 'x' in '-fweavec-checks=x'; expected trap, ...`.
static std::string invalidValue(llvm::StringRef arg, llvm::StringRef value,
                                llvm::StringRef expected) {
  return "invalid value '" + value.str() + "' in '" + arg.str() +
         "'; expected " + expected.str();
}

/// The WeaveC flags spelled `<flag>=<value>`. True if `arg` is one; `error`
/// then says what is wrong with it, if anything.
static bool consumeValueFlag(DriverOptions &options, llvm::StringRef arg,
                             std::string &error) {
  static constexpr std::array<llvm::StringLiteral, 7> Names{
      "-fweavec-analysis-stats", "-fweavec-checks",        "-fweavec-require",
      "-fweavec-ledger",         "-fweavec-ledger-format", "-fweavec-budget",
      "-fweavec-print-prelude"};
  const auto [flag, value] = arg.split('=');
  if (!llvm::is_contained(Names, flag))
    return false;
  if (flag == arg || value.empty()) {
    error = "missing value for '" + flag.str() + "'";
    return true;
  }
  if (flag == "-fweavec-print-prelude") {
    // Not forwarded: the driver prints the prelude and exits.
    if (value == "inline")
      options.printPrelude = PreludeForm::Inline;
    else if (value == "out-of-line")
      options.printPrelude = PreludeForm::OutOfLine;
    else
      error = invalidValue(arg, value, "inline or out-of-line");
    return true;
  }
  if (flag == "-fweavec-analysis-stats") {
    options.analysisStatsPath = value.str();
    if (!options.stats)
      options.stats = std::make_shared<core::AnalysisStats>();
  } else if (flag == "-fweavec-checks") {
    if (const std::optional<core::ChecksMode> mode =
            core::parseChecksMode(value))
      options.checks = *mode;
    else
      error = invalidValue(arg, value, "trap, report, verify or none");
  } else if (flag == "-fweavec-require") {
    if (const std::optional<core::RequireLevel> level =
            core::parseRequireLevel(value))
      options.require = *level;
    else
      error = invalidValue(arg, value, "none, guarded, checked or proven");
  } else if (flag == "-fweavec-ledger") {
    options.ledger = value.str();
  } else if (flag == "-fweavec-ledger-format") {
    if (const std::optional<LedgerFormat> format = parseLedgerFormat(value))
      options.ledgerFormat = *format;
    else
      error = invalidValue(arg, value, "json or sarif");
  } else if (flag == "-fweavec-budget") {
    std::uint64_t budget = 0;
    if (value.getAsInteger(10, budget))
      error = invalidValue(arg, value, "a number of block transfers");
    else
      options.budget = budget;
  }
  options.spellings.push_back(arg.str());
  return true;
}

bool DriverOptions::consume(llvm::StringRef arg, std::string &error) {
  if (arg == "-fweavec-print-prelude") {
    printPrelude = PreludeForm::Inline;
    return true;
  }
  if (consumeValueFlag(*this, arg, error))
    return true;
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
  static constexpr std::array<Flag, 5> Flags{{
      {.name = "weavec", .member = &DriverOptions::enabled},
      {.name = "weavec-dump-analysis", .member = &DriverOptions::dumpAnalysis},
      {.name = "weavec-link", .member = &DriverOptions::link},
      {.name = "weavec-stack-objects", .member = &DriverOptions::stackObjects},
      {.name = "weavec-global-objects",
       .member = &DriverOptions::globalObjects},
  }};
  // Switches whose default depends on other flags.
  struct Switch {
    llvm::StringLiteral name;
    std::optional<bool> DriverOptions::*member;
  };
  static constexpr std::array<Switch, 3> Switches{{
      {.name = "weavec-zero-init", .member = &DriverOptions::zeroInit},
      {.name = "weavec-summary", .member = &DriverOptions::summary},
      {.name = "weavec-runtime", .member = &DriverOptions::runtime},
  }};
  if (const auto *const found = llvm::find_if(
          Flags, [name](const Flag &f) { return name == f.name; });
      found != Flags.end()) {
    this->*found->member = value;
  } else if (const auto *const toggled = llvm::find_if(
                 Switches, [name](const Switch &s) { return name == s.name; });
             toggled != Switches.end()) {
    this->*toggled->member = value;
  } else {
    if (!name.starts_with("weavec"))
      return false;
    error = "unknown WeaveC flag '" + arg.str() + "'";
    return true;
  }
  spellings.push_back(arg.str());
  return true;
}

bool DriverOptions::zeroInitialises() const {
  return checks != core::ChecksMode::None && zeroInit.value_or(true);
}

FrontendOptions DriverOptions::toFrontendOptions() const {
  FrontendOptions options;
  options.engine.stats = stats.get();
  options.analysisStatsPath = analysisStatsPath;
  options.engine.dumpStream = dumpAnalysis ? &llvm::outs() : nullptr;
  // RFC 0030 §3.2, §11: the ledger describes the enforcing build, which
  // zero-initialises unless `-fno-weavec-zero-init` says otherwise.
  options.engine.zeroInit = zeroInit.value_or(true);
  options.engine.budget = budget;
  options.control = control;
  options.config = core::LedgerConfig{
      .checks = checks,
      .runtime = usesRuntime() ? core::RuntimeUse::On : core::RuntimeUse::Off,
      .zeroInit = zeroInitialises(),
      .require = require,
      .budget = budget};
  // RFC 0032 §9: a possible finding the build enforces is not a warning.
  options.dropGuardedPossible = ChecksAreEmitted && enforcesGuards() &&
                                !control.possibleFindings().value_or(false);
  options.stackObjects = stackObjects;
  options.globalObjects = globalObjects;
  options.ledgerOutput = LedgerOutputOptions{
      .path = ledger,
      .format = ledgerFormat,
      .summary = summary,
      .checksEnforced = ChecksAreEmitted && checks != core::ChecksMode::None};
  return options;
}

llvm::StringRef driverFlagsHelp() {
  return R"(WeaveC flags of weavec-cc (RFC 0030); every other flag is Clang's.

  -fweavec, -fno-weavec
      Analyse, check and zero-initialise (the default), or compile as plain
      Clang.
  -fweavec-checks=trap|report|verify|none
      What unproven spatial and null obligations become (default: trap).
  -fweavec-zero-init, -fno-weavec-zero-init
      Zero-initialise locals and heap allocations (default: on unless
      -fweavec-checks=none).
  -fweavec-runtime, -fno-weavec-runtime
      Guard unresolved facets against the runtime's object table, register
      stack and global objects, and link the runtime's allocator (default:
      on; off with sanitizers that replace the allocator, -ffreestanding,
      -nostdlib and unsupported targets).
  -fweavec-stack-objects, -fno-weavec-stack-objects
  -fweavec-global-objects, -fno-weavec-global-objects
      Register this unit's escaping locals, and its globals, with the
      runtime (default: on).
  -fweavec-require=none|guarded|checked|proven
      Make every unresolved facet an error (guarded), every guarded one too
      (checked), and every checked one too (proven). Default: none.
  -Wweavec-possible, -Wno-weavec-possible
      Report possible temporal findings even where the build guards the
      facet at run time (default: off in enforcing builds).
  -fweavec-ledger=<path>
      Write the unit ledger (compile) or the program ledger (link). A
      directory (a value ending in '/') receives <object>.ledger.json per
      unit and <output>.ledger.json per link; a file receives one ledger.
  -fweavec-ledger-format=json|sarif
      The ledger's format (default: json).
  -fweavec-summary, -fno-weavec-summary
      Print the summary line on stderr (default: when a ledger is written).
  -fweavec-budget=<n>
      Block transfers per function before its analysis stops (default:
      50000; 0: unlimited).
  -fweavec-link, -fno-weavec-link
      Run the whole-program step before linking (default: on).
  -fweavec-print-prelude
      Print the check prelude of the -fweavec-checks mode and exit.
  -fweavec-dump-analysis, -fweavec-analysis-stats=<path>
      Debugging output: the inferred facts, and work statistics as JSON.
  -Wno-weavec-<id>, -Wweavec-<id>, -Werror=weavec[-<id>],
  -Wno-error=weavec[-<id>], -Wweavec, -Wno-weavec
      Control WeaveC's diagnostics. Errors can be lowered, not disabled;
      -Wweavec-allocation-failure enables the one id that is off by default.
)";
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
  // RFC 0030, sections 10.2 and 10.9 (begin): a C code-generating action
  // with checks gets the check prelude in its predefines, unless a
  // precompiled header or modules are in use. Their predefines must stay
  // those plain Clang built them with, so the helpers are then declared
  // `extern` and come from libweavec_chk.a.
  bool BeginSourceFileAction(clang::CompilerInstance &compiler) override {
    if (!WrapperFrontendAction::BeginSourceFileAction(compiler))
      return false;
    if (!emitsChecks(compiler))
      return true;
    const clang::PreprocessorOptions &preprocessor =
        compiler.getPreprocessorOpts();
    const clang::LangOptions &lang = compiler.getLangOpts();
    externalHelpers = !preprocessor.ImplicitPCHInclude.empty() ||
                      lang.Modules || lang.CompilingPCH;
    if (externalHelpers)
      return true;
    PreludeOptions prelude;
    prelude.mode = preludeModeOf(options.config.checks);
    prelude.zeroInit = options.config.zeroInit;
    prelude.runtime = options.config.runtime == core::RuntimeUse::On;
    prelude.usableSize = usableSizeQueryFor(compiler.getTarget().getTriple());
    clang::Preprocessor &pp = compiler.getPreprocessor();
    pp.setPredefines(pp.getPredefines() + buildCheckPrelude(prelude));
    return true;
  }
  // RFC 0030, sections 10.2 and 10.9 (end).

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override {
    // RFC 0030, sections 10.5 and 10.6 (begin): the unit's planned ledger
    // and its zero-initialisation plan reach the check emitter through
    // `onResult`.
    auto planned = std::make_shared<UnitResult>();
    FrontendOptions analysisOptions = options;
    analysisOptions.onResult = [planned,
                                forward = options.onResult](UnitResult unit) {
      planned->ledger = unit.ledger;
      planned->zeroInit = unit.zeroInit;
      if (forward)
        forward(std::move(unit));
    };
    std::unique_ptr<clang::ASTConsumer> analysis =
        createWeaveCConsumer(compiler, analysisOptions);
    // RFC 0031 §9.2: a unit whose checks are emitted keeps its dispatch
    // blocks' predecessors duplicable. Registered before the code generator
    // is made, which reads the callbacks when it runs the pipeline.
    if (emitsChecks(compiler))
      registerDispatchEdgeSplit(compiler.getCodeGenOpts());
    std::unique_ptr<clang::ASTConsumer> inner =
        WrapperFrontendAction::CreateASTConsumer(compiler, inFile);
    if (!inner)
      return analysis;
    // A code-generating action gets the code generator behind
    // DeferredCodeGenConsumer, so the analysis and the check rewrites run
    // before any code is emitted; other actions keep the multiplexer.
    if (isCodeGenAction(compiler.getFrontendOpts().ProgramAction)) {
      const bool checks = emitsChecks(compiler);
      clang::DiagnosticsEngine &diagnostics = compiler.getDiagnostics();
      // The consumer the hook hands the declarations it adds to: itself,
      // which records them behind the unit's own (§10.5).
      auto self = std::make_shared<clang::ASTConsumer *>(nullptr);
      auto deferred = std::make_unique<DeferredCodeGenConsumer>(
          std::move(inner), [this, &diagnostics, checks, planned, self,
                             analysis = std::move(analysis)](
                                clang::ASTContext &context, clang::Sema &sema) {
            analysis->HandleTranslationUnit(context);
            // §10.5 step 2: the checks of a unit without errors.
            if (!checks || diagnostics.hasErrorOccurred())
              return;
            CheckEmitter emitter(
                sema, CheckEmitterOptions{.mode = options.config.checks,
                                          .externalHelpers = externalHelpers});
            if (planned->ledger)
              emitter.emit(*planned->ledger);
            if (planned->zeroInit)
              emitter.lowerZeroInit(*planned->zeroInit);
            // RFC 0032 §4, §5: the unit's stack and global objects.
            if (options.config.runtime == core::RuntimeUse::On &&
                planned->ledger && planned->ledger->sites) {
              emitter.registerObjects(
                  planObjects(context, *planned->ledger->sites,
                              core::LibrarySpec::shipped(),
                              ObjectOptions{.stack = options.stackObjects,
                                            .globals = options.globalObjects}));
              for (clang::Decl *added : emitter.newTopLevelDecls())
                (*self)->HandleTopLevelDecl(clang::DeclGroupRef(added));
            }
          });
      *self = deferred.get();
      return deferred;
    }
    // RFC 0030, sections 10.5 and 10.6 (end).
    std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
    consumers.push_back(std::move(analysis));
    consumers.push_back(std::move(inner));
    return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
  }

private:
  FrontendOptions options;
  /// §10.9: the prelude was not injected.
  bool externalHelpers = false;

  /// A C unit whose code is emitted, with checks on (§10.5, §10.7).
  [[nodiscard]] bool
  emitsChecks(const clang::CompilerInstance &compiler) const {
    const clang::LangOptions &lang = compiler.getLangOpts();
    // OpenCL C has no function pointers, which the prelude needs.
    return options.config.checks != core::ChecksMode::None && !lang.CPlusPlus &&
           !lang.ObjC && !lang.OpenCL &&
           isCodeGenAction(compiler.getFrontendOpts().ProgramAction);
  }

  static CheckMode preludeModeOf(core::ChecksMode checks) {
    switch (checks) {
    case core::ChecksMode::Trap:
      return CheckMode::Trap;
    case core::ChecksMode::Report:
      return CheckMode::Report;
    case core::ChecksMode::Verify:
      return CheckMode::Verify;
    case core::ChecksMode::None:
      return CheckMode::None;
    }
    return CheckMode::Trap;
  }
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

/// RFC 0030 §11: on a target whose C library has no usable-size query, the
/// allocation family is not zero-initialised; `weavec-cc` says so once.
static void warnOnceWithoutUsableSize(llvm::StringRef triple) {
  static bool warned = false;
  if (warned || usableSizeQueryFor(llvm::Triple(
                    llvm::Triple::normalize(triple))) != UsableSizeQuery::None)
    return;
  warned = true;
  llvm::errs() << "weavec-cc: warning: the C library of '" << triple
               << "' has no usable-size query, so heap allocations are not "
                  "zero-initialised\n";
}

/// RFC 0030 §13.1: writes the unit record of the object `output` the job
/// `compiler` just wrote.
static bool writeUnitRecord(llvm::StringRef path, llvm::StringRef output,
                            llvm::ArrayRef<const char *> cc1Args,
                            const clang::CompilerInstance &compiler,
                            const core::LedgerConfig &config,
                            const UnitResult &result, std::string *error) {
  record::UnitRecord unit;
  unit.header.producer = record::currentProducer();
  const auto &inputs = compiler.getFrontendOpts().Inputs;
  if (!inputs.empty() && inputs.front().isFile())
    unit.header.source = inputs.front().getFile().str();
  unit.header.cwd = compiler.getFileSystemOpts().WorkingDir;
  if (unit.header.cwd.empty())
    unit.header.cwd = currentDirectory();
  for (const char *arg : cc1Args)
    unit.header.command.emplace_back(arg);
  unit.header.target = compiler.getTargetOpts().Triple;
  unit.header.config = config;
  const std::optional<std::string> digest = record::fileDigest(output);
  if (!digest) {
    if (error != nullptr)
      *error = "cannot read '" + output.str() + "' to bind its WeaveC record";
    return false;
  }
  unit.header.object =
      record::RecordObject{.path = output.str(), .digest = *digest};
  unit.payload = record::toJson(record::payloadOf(result));
  return record::writeRecord(path, unit, error);
}

/// Runs one `-cc1` job. `driver` is set when the `weavec-cc` driver runs the
/// job in process; the driver then owns the statistics and writes them.
static int runCc1Job(llvm::ArrayRef<const char *> argv, const char *argv0,
                     const DriverOptions *driver = nullptr) {
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

  // RFC 0030 §11 (begin): in the enforcing modes locals are
  // zero-initialised, and the heap family where the C library can say how
  // large a block is.
  if (weavec.zeroInitialises() &&
      isCodeGenAction(compiler->getFrontendOpts().ProgramAction)) {
    compiler->getLangOpts().setTrivialAutoVarInit(
        clang::LangOptions::TrivialAutoVarInitKind::Zero);
    warnOnceWithoutUsableSize(compiler->getTargetOpts().Triple);
  }
  // RFC 0030 §11 (end).

  // The compile step sees the unit alone (RFC 0030 §5.1: calls into the
  // other units are unknown callees until the link step).
  if (driver != nullptr && driver->stats)
    weavec.stats = driver->stats;
  // RFC 0030 §13.1: a job that writes a file writes the unit record next
  // to it.
  const std::string output = compiler->getFrontendOpts().OutputFile;
  const bool writesRecord = !output.empty() && output != "-";
  FrontendOptions options = weavec.toFrontendOptions();
  options.collectInterface = writesRecord;
  const core::LedgerConfig config = options.config;
  std::optional<UnitResult> result;
  options.onResult = [&result](UnitResult r) { result = std::move(r); };

  WeaveCWrapperAction action(std::move(inner), std::move(options));
  success = compiler->ExecuteAction(action);

  if (writesRecord) {
    const std::string path = record::recordPathFor(output);
    std::string error;
    if (!success || !result) {
      removeQuietly(path);
    } else if (!writeUnitRecord(path, output, cc1Args, *compiler, config,
                                *result, &error)) {
      removeQuietly(path);
      llvm::errs() << "weavec-cc: warning: " << error << '\n';
    }
  }
  const bool statsOK =
      driver != nullptr ||
      writeAnalysisStats(weavec.analysisStatsPath, weavec.stats.get());
  return success && statsOK ? 0 : 1;
}

//===----------------------------------------------------------------------===//
// The link step
//===----------------------------------------------------------------------===//

namespace {

/// A unit re-parsed from the cc1 command line its record holds.
class Cc1Unit final : public ProgramUnit {
public:
  Cc1Unit(std::string displayName, std::vector<std::string> command,
          std::string workingDirectory, core::LedgerConfig recorded,
          const char *argv0)
      : display(std::move(displayName)), args(std::move(command)),
        cwd(std::move(workingDirectory)), config(recorded), argv0(argv0) {}

  [[nodiscard]] std::string name() const override { return display; }

  bool run(clang::tooling::FrontendActionFactory &factory) override {
    auto invocation = createInvocation();
    if (!invocation)
      return false;
    clang::CompilerInstance compiler(std::move(invocation));
    compiler.createDiagnostics();
    if (!compiler.hasDiagnostics())
      return false;
    const std::unique_ptr<clang::FrontendAction> action = factory.create();
    return compiler.ExecuteAction(*action);
  }

  bool analyze(const FrontendOptions &options) override {
    if (!attemptedParse) {
      attemptedParse = true;
      auto invocation = createInvocation();
      if (!invocation)
        return false;
      core::AnalysisTimer timer(options.engine.stats, "parsing");
      if (options.engine.stats)
        options.engine.stats->add("unit_parses");
      diagOptions = std::make_shared<clang::DiagnosticOptions>(
          invocation->getDiagnosticOpts());
      auto diagnostics = llvm::makeIntrusiveRefCnt<clang::DiagnosticsEngine>(
          llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), *diagOptions,
          new clang::TextDiagnosticPrinter(llvm::errs(), *diagOptions));
      ast = clang::ASTUnit::LoadFromCompilerInvocation(
          invocation, std::make_shared<clang::PCHContainerOperations>(),
          diagOptions, diagnostics,
          llvm::makeIntrusiveRefCnt<clang::FileManager>(
              invocation->getFileSystemOpts()));
      if (!ast || ast->getDiagnostics().hasErrorOccurred()) {
        ast.reset();
        return false;
      }
    } else if (options.engine.stats) {
      options.engine.stats->add("unit_reuses");
    }
    if (!ast)
      return false;
    auto current = options;
    // The unit is analysed as it was compiled.
    current.config = config;
    auto result = analyzeRetainedUnit(*ast, current);
    if (options.onResult)
      options.onResult(std::move(result));
    return true;
  }

  bool releaseAST() override {
    if (!ast)
      return false;
    ast.reset();
    diagOptions.reset();
    attemptedParse = false;
    return true;
  }

private:
  std::shared_ptr<clang::CompilerInvocation> createInvocation() const {
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
      return {};
    }
    if (!cwd.empty())
      invocation->getFileSystemOpts().WorkingDir = cwd;
    // Only the analysis runs; nothing is written.
    invocation->getFrontendOpts().OutputFile.clear();
    // RFC 0030 §16: whatever format the compile asked for, the re-analysis
    // renders text, which the link relays; a SARIF printer here would need
    // a document writer nobody attaches.
    invocation->getDiagnosticOpts().setFormat(clang::DiagnosticOptions::Clang);
    if (invocation->getHeaderSearchOpts().ResourceDir.empty())
      invocation->getHeaderSearchOpts().ResourceDir = getClangResourceDir();

    return invocation;
  }

  bool attemptedParse = false;
  std::shared_ptr<clang::DiagnosticOptions> diagOptions;
  std::unique_ptr<clang::ASTUnit> ast;
  std::string display;
  std::vector<std::string> args;
  std::string cwd;
  core::LedgerConfig config;
  const char *argv0;
};

/// A link input with a valid unit record (§13.1).
struct LinkInput {
  std::string object;
  record::UnitRecord record;
  record::Payload payload;
};

/// RFC 0030 §13.2: a link input without a valid WeaveC record.
struct UnanalyzedInput {
  /// The input as the command line names it: the source for an object
  /// compiled by the same invocation, the file a `-l` resolved to.
  std::string name;
  /// Why its record is not valid; empty when it has none.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string stale = {};
};

/// The inputs of one link: those with a valid record, and those without
/// one that are not the platform's own.
struct LinkInputs {
  std::vector<LinkInput> analysed;
  std::vector<UnanalyzedInput> unanalyzed;
};

} // namespace

/// `path` absolute, without `.` and `..`, and without symbolic links when it
/// exists.
static std::string canonicalPath(llvm::StringRef path) {
  llvm::SmallString<256> real;
  if (!llvm::sys::fs::real_path(path, real))
    return real.str().str();
  llvm::SmallString<256> absolute(path);
  std::ignore = llvm::sys::fs::make_absolute(absolute);
  llvm::sys::path::remove_dots(absolute, /*remove_dot_dot=*/true);
  return absolute.str().str();
}

/// Whether `path` is `root` or inside it; both canonical.
static bool isUnder(llvm::StringRef path, llvm::StringRef root) {
  return !root.empty() && path.starts_with(root) &&
         (path.size() == root.size() ||
          llvm::sys::path::is_separator(path[root.size()]));
}

/// The SDK or sysroot of the link: `--sysroot`, else `-isysroot`, which
/// `weavec-cc` passes on Apple platforms when the command line does not.
static std::string linkSysroot(const clang::driver::Compilation &compilation) {
  const llvm::opt::ArgList &args = compilation.getArgs();
  if (const llvm::opt::Arg *sysroot =
          args.getLastArg(clang::options::OPT__sysroot_EQ))
    return sysroot->getValue();
  if (const llvm::opt::Arg *sysroot =
          args.getLastArg(clang::options::OPT_isysroot))
    return sysroot->getValue();
  return compilation.getDriver().SysRoot;
}

/// §13.2, §5.2: the directories whose objects and libraries are the
/// platform's: the toolchain's file and library paths, its resource
/// directory, and the SDK or sysroot (`/usr/lib` and `/lib` without one).
static std::vector<std::string>
systemRoots(const clang::driver::Compilation &compilation) {
  const clang::driver::ToolChain &toolChain = compilation.getDefaultToolChain();
  std::vector<std::string> roots;
  const auto add = [&roots](llvm::StringRef path) {
    if (!path.empty() && path != "/")
      roots.push_back(canonicalPath(path));
  };
  for (const std::string &path : toolChain.getFilePaths())
    add(path);
  for (const std::string &path : toolChain.getLibraryPaths())
    add(path);
  add(compilation.getDriver().ResourceDir);
  const std::string sysroot = linkSysroot(compilation);
  if (!sysroot.empty() && sysroot != "/") {
    add(sysroot);
  } else {
    for (const llvm::StringRef path :
         {"/usr/lib", "/usr/lib64", "/lib", "/lib64"})
      add(path);
  }
  return roots;
}

/// The file `-l<name>` names, found as the linker finds it: the `-L`
/// directories in order, then the toolchain's file paths, then the SDK or
/// sysroot; in each directory the target's shared forms before the archive.
/// `-l:<file>` names the file itself.
static std::optional<std::string>
resolveLibrary(llvm::StringRef name,
               const clang::driver::Compilation &compilation) {
  const clang::driver::ToolChain &toolChain = compilation.getDefaultToolChain();
  std::vector<std::string> directories =
      compilation.getArgs().getAllArgValues(clang::options::OPT_L);
  for (const std::string &path : toolChain.getFilePaths())
    directories.push_back(path);
  const std::string sysroot = linkSysroot(compilation);
  for (const llvm::StringRef sub : {"usr/lib", "usr/local/lib", "lib"}) {
    llvm::SmallString<256> path(sysroot.empty() ? "/" : sysroot);
    llvm::sys::path::append(path, sub);
    directories.push_back(path.str().str());
  }
  std::vector<std::string> candidates;
  if (name.consume_front(":")) {
    candidates.push_back(name.str());
  } else {
    const std::string stem = "lib" + name.str();
    if (toolChain.getTriple().isOSDarwin())
      candidates = {stem + ".tbd", stem + ".dylib", stem + ".a"};
    else
      candidates = {stem + ".so", stem + ".a"};
  }
  for (const std::string &directory : directories) {
    for (const std::string &candidate : candidates) {
      llvm::SmallString<256> path(directory);
      llvm::sys::path::append(path, candidate);
      if (llvm::sys::fs::exists(path))
        return path.str().str();
    }
  }
  return std::nullopt;
}

/// Reads the record of every input on the link line, and names every other
/// input that is not the platform's (§13.2): objects, archives and shared
/// libraries without a record, records that are stale or unreadable, and
/// what `-l` finds outside the system directories. A `-l` the linker
/// cannot resolve is left to it.
static LinkInputs
collectLinkInputs(const clang::driver::Compilation &compilation,
                  const clang::driver::Command &link) {
  LinkInputs inputs;
  const std::vector<std::string> roots = systemRoots(compilation);
  const auto isSystem = [&roots](llvm::StringRef path) {
    const std::string canonical = canonicalPath(path);
    return llvm::any_of(roots, [&canonical](const std::string &root) {
      return isUnder(canonical, root);
    });
  };
  const auto isTemporary = [&compilation](llvm::StringRef path) {
    return llvm::any_of(compilation.getTempFiles(),
                        [path](const char *temporary) {
                          return path == llvm::StringRef(temporary);
                        });
  };
  const auto addUnanalyzed = [&inputs](UnanalyzedInput input) {
    if (llvm::none_of(inputs.unanalyzed, [&input](const UnanalyzedInput &seen) {
          return seen.name == input.name;
        }))
      inputs.unanalyzed.push_back(std::move(input));
  };
  for (const clang::driver::InputInfo &input : link.getInputInfos()) {
    // `-l` and other linker arguments are read from the command line below:
    // some toolchains (Darwin's) keep only the files among a link's inputs.
    if (!input.isFilename())
      continue;
    const std::string object = input.getFilename();
    const std::string name =
        isTemporary(object) ? std::string(input.getBaseInput()) : object;
    const std::string path = record::recordPathFor(object);
    if (!llvm::sys::fs::exists(path)) {
      if (!isSystem(object))
        addUnanalyzed(UnanalyzedInput{.name = name});
      continue;
    }
    // §13.1: a record that is not format 28 with this schema and a valid
    // digest, whose payload cannot be read, or that was written for another
    // object is stale.
    const auto stale = [&](const std::string &why) {
      std::string reported = "'";
      reported += path;
      reported += "': ";
      reported += why;
      addUnanalyzed(
          UnanalyzedInput{.name = name, .stale = std::move(reported)});
    };
    std::string reason;
    std::optional<record::UnitRecord> unit = record::readRecord(path, reason);
    if (!unit) {
      stale(reason);
      continue;
    }
    if (record::fileDigest(object) != unit->header.object.digest) {
      stale("it describes another object (digest mismatch)");
      continue;
    }
    std::optional<record::Payload> payload =
        record::payloadFromJson(unit->payload, unit->header.source, reason);
    if (!payload) {
      stale(reason);
      continue;
    }
    inputs.analysed.push_back(LinkInput{.object = object,
                                        .record = std::move(*unit),
                                        .payload = std::move(*payload)});
  }
  for (const llvm::opt::Arg *arg :
       compilation.getArgs().filtered(clang::options::OPT_l)) {
    const std::optional<std::string> library =
        resolveLibrary(arg->getValue(), compilation);
    if (library && !isSystem(*library))
      addUnanalyzed(UnanalyzedInput{.name = *library});
  }
  return inputs;
}

/// §13.2 step 1: the one `unanalyzed-input` warning of a link, naming every
/// input without a valid record; none when there is none.
static std::optional<core::Diagnostic>
unanalyzedInputDiagnostic(llvm::ArrayRef<UnanalyzedInput> inputs) {
  if (inputs.empty())
    return std::nullopt;
  const auto describe = [](const UnanalyzedInput &input) {
    return "link input '" + input.name +
           (input.stale.empty()
                ? "' has no WeaveC record"
                : "' has a stale WeaveC record (" + input.stale + ")");
  };
  core::Diagnostic diagnostic{.severity = core::Severity::Warning,
                              .certainty = core::Certainty::Definite,
                              .id = core::diag::UnanalyzedInput,
                              .message = {},
                              .location = {},
                              .notes = {},
                              .fixits = {}};
  if (inputs.size() == 1) {
    diagnostic.message =
        describe(inputs.front()) + "; calls into it are trusted";
  } else {
    diagnostic.message = std::to_string(inputs.size()) +
                         " link inputs have no WeaveC record; calls into "
                         "them are trusted";
    for (const UnanalyzedInput &input : inputs)
      diagnostic.addNote(describe(input), {});
  }
  return diagnostic;
}

/// §9.3, §13.2 step 1: what the link line says about code outside the
/// inputs.
static LinkShape linkShapeOf(const clang::driver::Compilation &compilation,
                             llvm::ArrayRef<UnanalyzedInput> unanalyzed) {
  const llvm::opt::ArgList &args = compilation.getArgs();
  LinkShape shape;
  shape.executable = !args.hasArg(clang::options::OPT_shared) &&
                     !args.hasArg(clang::options::OPT_r) &&
                     !args.hasArg(clang::options::OPT_dynamiclib) &&
                     !args.hasArg(clang::options::OPT_bundle);
  const auto exportsDynamic = [](llvm::StringRef flag) {
    return flag == "-export-dynamic" || flag == "--export-dynamic" ||
           flag == "-E" || flag == "-export_dynamic";
  };
  shape.exportDynamic = args.hasArg(clang::options::OPT_rdynamic);
  for (const llvm::opt::Arg *arg :
       args.filtered(clang::options::OPT_Wl_COMMA, clang::options::OPT_Xlinker))
    for (const char *value : arg->getValues())
      shape.exportDynamic = shape.exportDynamic || exportsDynamic(value);
  for (const UnanalyzedInput &input : unanalyzed)
    shape.inputsWithoutRecords.push_back(input.name);
  return shape;
}

/// The name the link output gives the program (§12.4, §16).
static std::string linkOutput(const clang::driver::Command &link) {
  const std::vector<std::string> &outputs = link.getOutputFilenames();
  return outputs.empty() ? std::string("a.out") : outputs.front();
}

/// RFC 0005, *weavec-cc*: whether a unit's view at compile time can differ
/// from the program's, so that it must be analysed again at link.
static bool needsAnalysis(llvm::ArrayRef<ProgramMember> members,
                          std::size_t index, const core::SlotSolution &slots) {
  const analysis::UnitExports &exports = members[index].payload.exports;
  const auto other = [&](auto &&predicate) {
    for (std::size_t o = 0; o < members.size(); ++o)
      if (o != index && predicate(members[o].payload.exports))
        return true;
    return false;
  };
  if (!exports.unknownCallees.empty())
    return true;
  // A callee or an indirect-call candidate in another unit.
  if (llvm::any_of(exports.imports, [&](const std::string &name) {
        return other([&](const analysis::UnitExports &unit) {
          const auto it = unit.functions.find(name);
          return it != unit.functions.end() && it->second.external;
        });
      }))
    return true;
  if (llvm::any_of(exports.indirectTypes, [&](const std::string &key) {
        return other([&](const analysis::UnitExports &unit) {
          return llvm::any_of(unit.functions, [&](const auto &entry) {
            return entry.second.addressTaken && entry.second.typeKey == key;
          });
        });
      }))
    return true;
  // RFC 0030 §9.3: an indirect call through a slot the program resolves.
  return llvm::any_of(
      members[index].payload.facts.slots.rows, [&](const core::SlotRow &row) {
        const std::optional<core::SlotKey> callee = row.slot.callee();
        return callee && !slots.targets(*callee).empty();
      });
}

/// The whole-program step (RFC 0030 §13.2): true if the link may proceed.
static bool runLinkStep(const clang::driver::Compilation &compilation,
                        const clang::driver::Command &link,
                        const DriverOptions &weavec, const char *argv0) {
  LinkInputs linkInputs = collectLinkInputs(compilation, link);
  LinkDiagnosticPrinter reporter("weavec-cc");
  FilteringSink sink(reporter, weavec.control);
  std::vector<core::Diagnostic> linkDiagnostics;
  // Step 1.
  if (const auto unanalyzed =
          unanalyzedInputDiagnostic(linkInputs.unanalyzed)) {
    sink.report(*unanalyzed);
    linkDiagnostics.push_back(*unanalyzed);
    if (sink.errors() != 0)
      return false;
  }
  std::vector<LinkInput> &inputs = linkInputs.analysed;
  FrontendOptions unitOptions = weavec.toFrontendOptions();
  const LedgerOutputOptions output = unitOptions.ledgerOutput;
  const bool compose = output.writesLedger() || output.printsSummary();
  // Without a record there is nothing to analyse; the program ledger, when
  // one is asked for, still names the inputs without records.
  if (inputs.empty() && !compose)
    return true;

  const std::string cwd = currentDirectory();
  std::vector<ProgramMember> members;
  members.reserve(inputs.size());
  for (LinkInput &input : inputs)
    members.push_back(ProgramMember{.source = input.record.header.source,
                                    .cwd = input.record.header.cwd,
                                    .object = input.object,
                                    .target = input.record.header.target,
                                    .payload = std::move(input.payload)});
  const LinkShape shape = linkShapeOf(compilation, linkInputs.unanalyzed);

  // Step 2: the slots of every record, solved together.
  auto facts = std::make_shared<analysis::ProgramFacts>();
  facts->slots = solveProgramSlots(members, shape);
  facts->boundaries = programBoundaries(members);

  // Step 3: declarations against definitions.
  const DeclarationCheck declarations = verifyDeclarations(members, cwd);
  for (const core::Diagnostic &diagnostic : declarations.diagnostics)
    sink.report(diagnostic);

  // Step 4: the units analysed again with the program in view. The units'
  // ledgers and summary lines are the compile step's; the link writes only
  // the program's. Composing that ledger needs every unit's rows in full,
  // which records do not carry, so then every unit runs again.
  unitOptions.ledgerOutput.path.clear();
  unitOptions.ledgerOutput.summary = false;
  ProgramAnalysis program(std::move(unitOptions));
  program.setProgramFacts(facts);
  program.keepLedgers(compose);
  std::vector<std::optional<std::size_t>> analysed(inputs.size());
  std::size_t added = 0;
  std::set<core::RuntimeUse> runtimeUses;
  std::string firstGuarded;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const record::RecordHeader &header = inputs[i].record.header;
    runtimeUses.insert(header.config.runtime);
    if (firstGuarded.empty() && header.config.runtime == core::RuntimeUse::On)
      firstGuarded = inputs[i].object;
    const record::Payload &payload = members[i].payload;
    if ((compose || needsAnalysis(members, i, facts->slots)) &&
        !header.command.empty()) {
      const std::string name =
          header.source.empty() ? inputs[i].object : header.source;
      program.addUnit(std::make_unique<Cc1Unit>(name, header.command,
                                                header.cwd, header.config,
                                                argv0),
                      payload.exports, payload.reported);
      analysed[i] = added++;
    } else if (!header.command.empty() &&
               llvm::any_of(payload.exports.functions, [](const auto &entry) {
                 return entry.second.external || entry.second.addressTaken;
               })) {
      // Another unit may ask a context of its functions (RFC 0031 §7).
      const std::string name =
          header.source.empty() ? inputs[i].object : header.source;
      program.addServingUnit(std::make_unique<Cc1Unit>(name, header.command,
                                                       header.cwd,
                                                       header.config, argv0),
                             payload.exports, payload.reported);
      ++added;
    } else {
      program.addExports(payload.exports);
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

  // Step 5: the exported requirements decided at the callers in other
  // units, and the allocator (the rest of the step is part of the program
  // ledger).
  const RequirementCheck requirements = verifyRequirements(members, shape);
  // RFC 0030 §2.2: a violated requirement is an error, at the call (the
  // caller's own run cannot see the callee's requirement, RFC 0031 §6.1).
  for (const RequirementDecision &decision : requirements.decisions) {
    if (decision.decision.outcome != core::SiteOutcome::Violation)
      continue;
    std::optional<core::SourceLocation> at;
    if (const auto import = members[decision.member].payload.facts.imports.find(
            decision.callee);
        import != members[decision.member].payload.facts.imports.end())
      for (const record::ImportCall &call : import->second.calls)
        if (call.function == decision.function && call.site == decision.site)
          at = call.location;
    if (!at)
      continue;
    core::Diagnostic diagnostic;
    diagnostic.id = core::diag::OutOfBounds;
    diagnostic.severity = core::Severity::Error;
    diagnostic.message = decision.decision.detail;
    diagnostic.location = *at;
    sink.report(diagnostic);
    linkDiagnostics.push_back(std::move(diagnostic));
  }
  if (const std::optional<AllocatorFinding> allocator =
          allocatorDefinedBy(members))
    llvm::errs() << "weavec-cc: warning: "
                 << allocatorWarning(members, *allocator, cwd) << '\n';
  // RFC 0032 §2.6: guards compiled into a unit ask an object table the link
  // leaves without its allocator.
  if (!weavec.usesRuntime() && !firstGuarded.empty())
    llvm::errs() << "weavec-cc: note: linking without the WeaveC runtime, but '"
                 << firstGuarded
                 << "' was compiled with it: the heap is untracked, its "
                    "guards pass on it and releases are not validated "
                    "(RFC 0032)\n";

  // Step 6.
  bool written = true;
  if (compose) {
    std::vector<const core::Ledger *> runs(members.size(), nullptr);
    for (std::size_t i = 0; i < members.size(); ++i)
      if (analysed[i])
        runs[i] = program.ledgerOf(*analysed[i]);
    core::Ledger ledger = composeProgramLedger(
        ProgramLedgerInput{.members = members,
                           .runs = runs,
                           .copyRecordRows = true,
                           .declarations = &declarations,
                           .requirements = &requirements,
                           .shape = shape,
                           .linkDiagnostics = std::move(linkDiagnostics),
                           .cwd = cwd});
    applyDiagnosticControl(ledger, weavec.control);
    std::string error;
    // RFC 0032 §10: each unit's rows follow the flags it was compiled with;
    // the program's `config.runtime` says whether they all agree. A link
    // without the runtime has no allocator to guard against, whatever its
    // units were compiled with: the program's guards are not enforced.
    core::LedgerConfig programConfig = weavec.toFrontendOptions().config;
    if (programConfig.runtime != core::RuntimeUse::Off && !runtimeUses.empty())
      programConfig.runtime = runtimeUses.size() > 1 ? core::RuntimeUse::Mixed
                                                     : *runtimeUses.begin();
    written = emitProgramLedger(ledger, linkOutput(link), cwd, programConfig,
                                output, llvm::errs(), &error);
    if (!written)
      llvm::errs() << "weavec-cc: error: cannot write the WeaveC program "
                      "ledger: "
                   << error << '\n';
  }
  return result.ok() && sink.errors() == 0 && written;
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

/// The prelude's name for a `-fweavec-checks` mode.
static CheckMode preludeMode(core::ChecksMode checks) {
  switch (checks) {
  case core::ChecksMode::Trap:
    return CheckMode::Trap;
  case core::ChecksMode::Report:
    return CheckMode::Report;
  case core::ChecksMode::Verify:
    return CheckMode::Verify;
  case core::ChecksMode::None:
    return CheckMode::None;
  }
  return CheckMode::Trap;
}

/// `-fweavec-print-prelude` (§10.9): the prelude of the `-fweavec-checks`
/// mode, with the zero-initialisation helpers unless it is off, for the
/// target of `--target=` or `-target` (the default one otherwise), to the
/// file of `-o` or to stdout. Every other argument is ignored.
static int printPrelude(const DriverOptions &weavec,
                        llvm::ArrayRef<const char *> args) {
  std::string triple = llvm::sys::getDefaultTargetTriple();
  std::string output = "-";
  for (std::size_t i = 1; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (arg.consume_front("--target="))
      triple = arg.str();
    else if ((arg == "-target" || arg == "--target") && i + 1 < args.size())
      triple = args[++i];
    else if (arg == "-o" && i + 1 < args.size())
      output = args[++i];
  }
  PreludeOptions options;
  options.mode = preludeMode(weavec.checks);
  options.zeroInit = weavec.zeroInitialises();
  options.runtime = weavec.usesRuntime();
  options.usableSize =
      usableSizeQueryFor(llvm::Triple(llvm::Triple::normalize(triple)));
  options.form = weavec.printPrelude.value_or(PreludeForm::Inline);
  std::error_code error;
  llvm::raw_fd_ostream out(output, error);
  if (error) {
    llvm::errs() << "weavec-cc: error: cannot write '" << output
                 << "': " << error.message() << '\n';
    return 1;
  }
  out << buildCheckPrelude(options);
  return 0;
}

/// The ledgers one invocation writes (§16): one per compile job of a C
/// source, and one at the link when the whole-program step runs.
static std::size_t countLedgers(const clang::driver::Compilation &compilation,
                                const DriverOptions &weavec) {
  std::size_t count = 0;
  for (const clang::driver::Command &job : compilation.getJobs()) {
    const clang::driver::Action::ActionClass kind = job.getSource().getKind();
    if (kind == clang::driver::Action::LinkJobClass) {
      count += weavec.link ? 1 : 0;
      continue;
    }
    if (job.getArguments().empty() ||
        llvm::StringRef(job.getArguments().front()) != "-cc1" ||
        kind == clang::driver::Action::PreprocessJobClass ||
        kind == clang::driver::Action::PrecompileJobClass)
      continue;
    if (llvm::any_of(job.getInputInfos(),
                     [](const clang::driver::InputInfo &input) {
                       return input.getType() == clang::driver::types::TY_C ||
                              input.getType() == clang::driver::types::TY_PP_C;
                     }))
      ++count;
  }
  return count;
}

/// Clang's Darwin link job names `<weavec-cc>/../lib/libLTO.dylib` as
/// `-lto_library`, which is not installed beside weavec-cc (and current
/// linkers warn about it): without it the linker uses its own, as it did
/// when it ignored the missing one. (Not the one of the Clang WeaveC was
/// built with: objects built by the system compiler, the runtime archives
/// among them, may carry bitcode only the linker's own reads.)
static void dropMissingLtoLibrary(clang::driver::Compilation &compilation) {
  for (clang::driver::Command &job : compilation.getJobs()) {
    if (job.getSource().getKind() != clang::driver::Action::LinkJobClass)
      continue;
    const llvm::opt::ArgStringList &old = job.getArguments();
    llvm::opt::ArgStringList args;
    for (std::size_t i = 0; i < old.size(); ++i) {
      if (llvm::StringRef(old[i]) == "-lto_library" && i + 1 < old.size() &&
          !llvm::sys::fs::exists(old[i + 1])) {
        ++i;
        continue;
      }
      args.push_back(old[i]);
    }
    if (args.size() != old.size())
      job.replaceArguments(args);
  }
}

/// RFC 0032 §2.6: why the unit cannot use the runtime, from the command
/// line alone; empty when it can.
static std::string runtimeObstacle(llvm::ArrayRef<const char *> args) {
  std::string triple = llvm::sys::getDefaultTargetTriple();
  for (std::size_t i = 1; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (arg == "-ffreestanding")
      return "-ffreestanding has no hosted C library";
    if (arg == "-nostdlib" || arg == "-nodefaultlibs" || arg == "-nolibc")
      return arg.str() + " links no C library";
    if (arg.starts_with("-fsanitize=")) {
      llvm::SmallVector<llvm::StringRef, 8> kinds;
      arg.drop_front(sizeof("-fsanitize=") - 1).split(kinds, ',');
      for (const llvm::StringRef kind : kinds)
        if (kind == "address" || kind == "hwaddress" || kind == "memory" ||
            kind == "thread" || kind == "leak" || kind == "kernel-address")
          return "-fsanitize=" + kind.str() + " replaces the allocator";
    }
    if (arg.consume_front("--target="))
      triple = arg.str();
    else if ((arg == "-target" || arg == "--target") && i + 1 < args.size())
      triple = args[++i];
  }
  const llvm::Triple target(llvm::Triple::normalize(triple));
  if (!target.isArch64Bit() || !(target.isOSDarwin() || target.isOSLinux()))
    return "the runtime supports 64-bit Darwin and Linux targets";
  return {};
}

/// The archives of an enforcing link, and whether the link carries the
/// runtime's allocator.
namespace {
struct RuntimeArchives {
  std::string helpers;
  std::string runtime;
  std::string allocator;
};
} // namespace

/// RFC 0030 §10.7, §10.9 and RFC 0032 §7: adds the runtime archives to every
/// link job, before the first library on its line: a linker takes a symbol
/// from the first library that defines it, and scans an archive only for
/// what is undefined so far.
///
///   libweavec_alloc.a  the image's allocator (RFC 0032 §2.5), forced in by
///                      `-u malloc` unless the runtime is off
///   libweavec_chk.a    out-of-line helpers, for units built with a
///                      precompiled header or modules
///   libweavec_rt.a     the object table, the guards and the reports
///
/// The archives are host code, so they are added only when the link targets
/// the host; report mode cannot do without its runtime.
static bool addRuntimeLibraries(clang::driver::Compilation &compilation,
                                const DriverOptions &weavec, const char *argv0,
                                void *mainAddress) {
  const llvm::Triple target = compilation.getDefaultToolChain().getTriple();
  const llvm::Triple host(llvm::sys::getProcessTriple());
  const bool native =
      target.getArch() == host.getArch() && target.getOS() == host.getOS();
  const bool report = weavec.checks == core::ChecksMode::Report;
  RuntimeArchives archives;
  if (native) {
    archives.helpers =
        findRuntimeLibrary(argv0, mainAddress, "libweavec_chk.a");
    archives.runtime = findRuntimeLibrary(argv0, mainAddress, "libweavec_rt.a");
    if (weavec.enforcesGuards())
      archives.allocator =
          findRuntimeLibrary(argv0, mainAddress, "libweavec_alloc.a");
  }
  if (archives.runtime.empty() && (report || weavec.enforcesGuards())) {
    if (!native && !report)
      return true;
    llvm::errs() << "weavec-cc: error: cannot find libweavec_rt.a, which "
                 << (report ? "-fweavec-checks=report links"
                            : "an enforcing link carries (-fno-weavec-runtime "
                              "builds without it)")
                 << '\n';
    return false;
  }
  if (archives.helpers.empty() && archives.runtime.empty())
    return true;
  const auto owned = [&compilation](const std::string &text) {
    return compilation.getArgs().MakeArgString(text);
  };
  for (clang::driver::Command &job : compilation.getJobs()) {
    if (job.getSource().getKind() != clang::driver::Action::LinkJobClass)
      continue;
    const llvm::opt::ArgStringList &old = job.getArguments();
    llvm::opt::ArgStringList added;
    if (!archives.allocator.empty()) {
      added.push_back("-u");
      added.push_back(target.isOSDarwin() ? "_malloc" : "malloc");
      added.push_back(owned(archives.allocator));
    }
    if (!archives.helpers.empty())
      added.push_back(owned(archives.helpers));
    if (!archives.runtime.empty())
      added.push_back(owned(archives.runtime));
    // The runtime's thread-local stack list and its fork handlers.
    if (!archives.runtime.empty() && target.isOSLinux() && !target.isAndroid())
      added.push_back("-lpthread");
    // The first library of the line: `-l<name>`, which Darwin's
    // `-lto_library <path>` is not.
    const auto *const firstLibrary = llvm::find_if(old, [](const char *arg) {
      const llvm::StringRef text(arg);
      return text.starts_with("-l") && text != "-lto_library";
    });
    llvm::opt::ArgStringList args(old.begin(), firstLibrary);
    args.append(added.begin(), added.end());
    args.append(firstLibrary, old.end());
    job.replaceArguments(args);
  }
  return true;
}

/// Calls `visit(file)` for the object at `path`, or for each object member of
/// the archive there, until one answers true. A linked image (a shared
/// library, an executable) is not visited: its definitions are its own.
template <typename Visit>
static bool anyRelocatableObject(const std::string &path, const Visit &visit) {
  if (!llvm::sys::fs::is_regular_file(path))
    return false;
  llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> binary =
      llvm::object::createBinary(path);
  if (!binary) {
    llvm::consumeError(binary.takeError());
    return false;
  }
  if (const auto *file =
          llvm::dyn_cast<llvm::object::SymbolicFile>(binary->getBinary())) {
    const auto *object = llvm::dyn_cast<llvm::object::ObjectFile>(file);
    if (object != nullptr && !object->isRelocatableObject())
      return false;
    return visit(*file);
  }
  const auto *archive =
      llvm::dyn_cast<llvm::object::Archive>(binary->getBinary());
  if (archive == nullptr)
    return false;
  bool found = false;
  llvm::Error error = llvm::Error::success();
  for (const llvm::object::Archive::Child &child : archive->children(error)) {
    llvm::Expected<std::unique_ptr<llvm::object::Binary>> member =
        child.getAsBinary();
    if (!member) {
      llvm::consumeError(member.takeError());
      continue;
    }
    if (const auto *file =
            llvm::dyn_cast<llvm::object::SymbolicFile>(member->get());
        file != nullptr && visit(*file)) {
      found = true;
      break;
    }
  }
  llvm::consumeError(std::move(error));
  return found;
}

/// Calls `visit(name, weak)` for each global definition of `file` until one
/// answers true.
template <typename Visit>
static bool anyGlobalDefinition(const llvm::object::SymbolicFile &file,
                                const Visit &visit) {
  for (const llvm::object::BasicSymbolRef &symbol : file.symbols()) {
    llvm::Expected<std::uint32_t> flags = symbol.getFlags();
    if (!flags) {
      llvm::consumeError(flags.takeError());
      continue;
    }
    if ((*flags & llvm::object::BasicSymbolRef::SF_Undefined) != 0 ||
        (*flags & llvm::object::BasicSymbolRef::SF_Global) == 0)
      continue;
    std::string name;
    llvm::raw_string_ostream os(name);
    if (llvm::Error error = symbol.printName(os)) {
      llvm::consumeError(std::move(error));
      continue;
    }
    if (visit(name, (*flags & llvm::object::BasicSymbolRef::SF_Weak) != 0))
      return true;
  }
  return false;
}

/// RFC 0032 §2.6: the first input of a link, an object or a member of an
/// archive, that defines the allocator itself; empty when none does. The
/// allocator is what libweavec_alloc.a defines strongly: a second definition
/// of one of those could not be linked with it.
static std::string allocatorDefinedBy(const clang::driver::Command &link,
                                      const std::string &ownAllocator) {
  llvm::StringSet<> allocator;
  (void)anyRelocatableObject(
      ownAllocator, [&](const llvm::object::SymbolicFile &file) {
        return anyGlobalDefinition(file,
                                   [&](const std::string &name, bool weak) {
                                     if (!weak)
                                       allocator.insert(name);
                                     return false;
                                   });
      });
  if (allocator.empty())
    return {};
  for (const clang::driver::InputInfo &input : link.getInputInfos()) {
    if (!input.isFilename())
      continue;
    const std::string path = input.getFilename();
    if (path == ownAllocator)
      continue;
    const bool defines =
        anyRelocatableObject(path, [&](const llvm::object::SymbolicFile &file) {
          return anyGlobalDefinition(
              file, [&](const std::string &name, bool /*weak*/) {
                return allocator.contains(name);
              });
        });
    if (defines)
      return path;
  }
  return {};
}

/// RFC 0032 §2.6: a program that defines the allocator keeps it. Takes
/// libweavec_alloc.a (and the `-u` that forces it) off the link line, and
/// says once what that means.
static void
dropAllocatorIfDefined(const clang::driver::Compilation &compilation,
                       clang::driver::Command &link) {
  const llvm::opt::ArgStringList &old = link.getArguments();
  const auto *const own = llvm::find_if(old, [](const char *arg) {
    return llvm::sys::path::filename(arg) == "libweavec_alloc.a";
  });
  if (own == old.end())
    return;
  const std::string definer = allocatorDefinedBy(link, *own);
  if (definer.empty())
    return;
  llvm::opt::ArgStringList args;
  for (const auto *it = old.begin(); it != old.end(); ++it) {
    // `-u <malloc>` is the two arguments before the archive.
    if (it + 2 == own && llvm::StringRef(*it) == "-u") {
      ++it;
      continue;
    }
    if (it == own)
      continue;
    args.push_back(*it);
  }
  link.replaceArguments(args);
  (void)compilation;
  llvm::errs() << "weavec-cc: note: '" << definer
               << "' defines the allocator, so the WeaveC runtime's is not "
                  "linked: the heap is untracked, guards pass on it and "
                  "releases are not validated (RFC 0032)\n";
}

int runCc1(llvm::ArrayRef<const char *> argv, const char *argv0) {
  return runCc1Job(argv, argv0);
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
    if (text == "--help-weavec") {
      llvm::outs() << driverFlagsHelp();
      return 0;
    }
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
  if (weavec.printPrelude)
    return printPrelude(weavec, clangArgs);
  // RFC 0032 §2.6: builds the runtime cannot serve go on without it, as
  // `-fno-weavec-runtime` would; the link says so.
  std::string runtimeOff;
  if (weavec.enabled && weavec.checks != core::ChecksMode::None &&
      !weavec.runtime) {
    runtimeOff = runtimeObstacle(clangArgs);
    if (!runtimeOff.empty()) {
      weavec.runtime = false;
      weavec.spellings.emplace_back("-fno-weavec-runtime");
    }
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
  const auto cc1 = [&weavec](llvm::SmallVectorImpl<const char *> &args) {
    return runCc1Job(llvm::ArrayRef(args).drop_front(2), args[0], &weavec);
  };
  driver.CC1Main = cc1;
  if (const std::string resources = getClangResourceDir(); !resources.empty())
    driver.ResourceDir = resources;

  initializeTargets();
  const std::unique_ptr<clang::driver::Compilation> compilation(
      driver.BuildCompilation(clangArgs));
  if (!compilation || diags.hasErrorOccurred())
    return 1;
  if (weavec.enabled && !weavec.ledger.empty() &&
      !isLedgerDirectory(weavec.ledger)) {
    if (const std::size_t count = countLedgers(*compilation, weavec);
        count > 1) {
      llvm::errs() << "weavec-cc: error: '-fweavec-ledger=" << weavec.ledger
                   << "' would receive " << count
                   << " ledgers; name a directory (ending in '/') to get one "
                      "ledger per unit and per link\n";
      return 1;
    }
  }
  // RFC 0030 §10.7, §10.9 and RFC 0032 §7: every enforcing link carries the
  // runtime archives (an archive member is linked only when referenced).
  if (weavec.enabled && weavec.checks != core::ChecksMode::None &&
      !addRuntimeLibraries(*compilation, weavec, argv[0], mainAddress))
    return 1;
  const bool links = llvm::any_of(
      compilation->getJobs(), [](const clang::driver::Command &job) {
        return job.getSource().getKind() == clang::driver::Action::LinkJobClass;
      });
  if (!runtimeOff.empty() && links)
    llvm::errs() << "weavec-cc: note: building without the WeaveC runtime ("
                 << runtimeOff << "): guardable facets stay unresolved\n";
  dropMissingLtoLibrary(*compilation);
  if (printJobsOnly) {
    compilation->getJobs().Print(llvm::errs(), "\n", /*Quote=*/true);
    return 0;
  }

  // Every cc1 job runs here, whatever their number, so that a WeaveC flag,
  // a record and a diagnostic mean the same thing in a one-step build as
  // in a `-c` build.
  for (clang::driver::Command &job : compilation->getJobs()) {
    if (!job.getArguments().empty() &&
        llvm::StringRef(job.getArguments().front()) == "-cc1")
      job.InProcess = true;
  }

  int status = 0;
  for (clang::driver::Command &job : compilation->getJobs()) {
    const bool isLink =
        job.getSource().getKind() == clang::driver::Action::LinkJobClass;
    // The objects exist now: a program-defined allocator is visible.
    if (isLink && weavec.enabled)
      dropAllocatorIfDefined(*compilation, job);
    if (isLink && weavec.enabled && weavec.link &&
        !runLinkStep(*compilation, job, weavec, executable.c_str())) {
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

  // Temporary objects vanish with the compilation; so should their records.
  for (const char *temp : compilation->getTempFiles())
    removeQuietly(record::recordPathFor(temp));
  const bool statsOK =
      writeAnalysisStats(weavec.analysisStatsPath, weavec.stats.get());
  return statsOK ? status : 1;
}

} // namespace weavec::frontend
