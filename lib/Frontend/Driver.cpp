//===- Driver.cpp - weavec-cc, the compiler driver ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Driver.h"

#include "weavec/Config/Version.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/EnforcementLedger.h"
#include "weavec/Frontend/FrontendAction.h"
#include "weavec/Frontend/GuardPass.h"
#include "weavec/Frontend/ResourceDir.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/Sanitizers.h"
#include "clang/Driver/Action.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/InputInfo.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Driver/Types.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/FrontendTool/Utils.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolicFile.h"
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
#include <memory>
#include <optional>
#include <string>
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
  static constexpr std::array<llvm::StringLiteral, 5> Names{
      "-fweavec-analysis-stats", "-fweavec-checks", "-fweavec-ledger",
      "-fweavec-budget", "-fweavec-unit-budget"};
  const auto [flag, value] = arg.split('=');
  if (!llvm::is_contained(Names, flag))
    return false;
  if (flag == arg || value.empty()) {
    error = "missing value for '" + flag.str() + "'";
    return true;
  }
  if (flag == "-fweavec-analysis-stats") {
    options.analysisStatsPath = value.str();
    if (!options.stats)
      options.stats = std::make_shared<core::AnalysisStats>();
  } else if (flag == "-fweavec-checks") {
    if (value == "trap")
      options.checks = DriverOptions::Checks::Trap;
    else if (value == "report")
      options.checks = DriverOptions::Checks::Report;
    else if (value == "verify")
      options.checks = DriverOptions::Checks::Verify;
    else if (value == "none")
      options.checks = DriverOptions::Checks::None;
    else
      error = invalidValue(arg, value, "trap, report, verify or none");
  } else if (flag == "-fweavec-ledger") {
    options.ledger = value.str();
  } else {
    std::uint64_t budget = 0;
    if (value.getAsInteger(10, budget))
      error = invalidValue(arg, value, "a number");
    else if (flag == "-fweavec-budget")
      options.budget = budget;
    else
      options.unitBudget = budget;
  }
  options.spellings.push_back(arg.str());
  return true;
}

bool DriverOptions::consume(llvm::StringRef arg, std::string &error) {
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
  if (name == "weavec") {
    enabled = value;
  } else if (name == "weavec-diagnose") {
    diagnose = value;
  } else if (name == "weavec-dump-analysis") {
    dumpAnalysis = value;
  } else if (name == "weavec-zero-init") {
    zeroInit = value;
  } else if (name == "weavec-summary") {
    summary = value;
  } else {
    if (!name.starts_with("weavec"))
      return false;
    error = "unknown WeaveC flag '" + arg.str() + "'";
    return true;
  }
  spellings.push_back(arg.str());
  return true;
}

llvm::StringRef driverFlagsHelp() {
  return R"(WeaveC flags of weavec-cc (RFC 0035); every other flag is Clang's.

  -fweavec, -fno-weavec
      Guard memory accesses (the default), or compile as plain Clang.
  -fweavec-checks=trap|report|verify|none
      What a failed guard does: trap (the default); report it once per site
      and continue; trap, with every guard a local proof removed kept as a
      monitor (verify); or nothing is guarded (none).
  -fweavec-zero-init, -fno-weavec-zero-init
      Zero-initialise locals (default: on unless -fweavec-checks=none).
  -fweavec-ledger=<path>
      Write the unit's enforcement ledger: what was guarded, proven or left
      unguarded, access by access. A directory (a value ending in '/')
      receives <object>.ledger.json per unit; a file receives one ledger.
  -fweavec-summary, -fno-weavec-summary
      Print the summary line on stderr (default: when a ledger is written).
  -fweavec-diagnose
      Also run WeaveC's ownership and lifetime analysis and print what it
      finds as warnings (-Werror=weavec makes them errors).
  -fweavec-budget=<n>, -fweavec-unit-budget=<n>
      The analysis's work budgets per function and per unit.
  -fweavec-dump-analysis, -fweavec-analysis-stats=<path>
      Debugging output of the analysis.
  -Wno-weavec-<id>, -Wweavec-<id>, -Werror=weavec[-<id>],
  -Wno-error=weavec[-<id>], -Wweavec, -Wno-weavec
      Control the analysis's diagnostics.
)";
}

//===----------------------------------------------------------------------===//
// The -cc1 step
//===----------------------------------------------------------------------===//

/// Whether `action` generates code, so that the guard pass runs.
static bool isCodeGenAction(clang::frontend::ActionKind action) {
  switch (action) {
  case clang::frontend::EmitAssembly:
  case clang::frontend::EmitBC:
  case clang::frontend::EmitLLVM:
  case clang::frontend::EmitLLVMOnly:
  case clang::frontend::EmitCodeGenOnly:
  case clang::frontend::EmitObj:
    return true;
  default:
    return false;
  }
}

namespace {

/// Clang's action for the job with the unsafe-region collector and, under
/// `-fweavec-diagnose`, the analysis running beside its code generator.
class WeaveCWrapperAction final : public clang::WrapperFrontendAction {
public:
  WeaveCWrapperAction(std::unique_ptr<clang::FrontendAction> wrapped,
                      std::shared_ptr<UnsafeRegions> unsafe,
                      std::optional<FrontendOptions> analysis)
      : WrapperFrontendAction(std::move(wrapped)), unsafe(std::move(unsafe)),
        analysis(std::move(analysis)) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override {
    std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
    if (unsafe)
      consumers.push_back(createUnsafeRegionCollector(compiler, unsafe));
    if (analysis)
      consumers.push_back(createWeaveCConsumer(compiler, *analysis));
    std::unique_ptr<clang::ASTConsumer> inner =
        WrapperFrontendAction::CreateASTConsumer(compiler, inFile);
    if (!inner)
      return nullptr;
    // The code generator last: the collector's ranges exist when it runs the
    // pass at the end of the unit.
    consumers.push_back(std::move(inner));
    return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
  }

private:
  std::shared_ptr<UnsafeRegions> unsafe;
  std::optional<FrontendOptions> analysis;
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

/// The analysis's options under `-fweavec-diagnose` (RFC 0035 §8): every
/// finding a warning.
static FrontendOptions analysisOptions(const DriverOptions &weavec) {
  FrontendOptions options;
  options.engine.stats = weavec.stats.get();
  options.analysisStatsPath = weavec.analysisStatsPath;
  options.engine.dumpStream = weavec.dumpAnalysis ? &llvm::outs() : nullptr;
  options.engine.zeroInit = weavec.zeroInitialises();
  if (weavec.budget)
    options.engine.budget = *weavec.budget;
  options.engine.unitBudget = weavec.unitBudget;
  // Warnings, unless the command line's own -W flags, applied after, say
  // otherwise.
  DiagnosticControl control;
  control.turnOffByDefault(core::diag::Leak);
  std::string ignored;
  (void)control.parse("-Wno-error=weavec", ignored);
  for (const std::string &flag : weavec.spellings)
    if (DiagnosticControl::isWeaveCFlag(flag))
      (void)control.parse(flag, ignored);
  options.control = std::move(control);
  return options;
}

/// The guard pass's mode for a `-fweavec-checks` mode.
static GuardOptions::Mode guardMode(DriverOptions::Checks checks) {
  switch (checks) {
  case DriverOptions::Checks::Report:
    return GuardOptions::Mode::Report;
  case DriverOptions::Checks::Verify:
    return GuardOptions::Mode::Verify;
  default:
    return GuardOptions::Mode::Trap;
  }
}

static llvm::StringRef checksName(DriverOptions::Checks checks) {
  switch (checks) {
  case DriverOptions::Checks::Trap:
    return "trap";
  case DriverOptions::Checks::Report:
    return "report";
  case DriverOptions::Checks::Verify:
    return "verify";
  case DriverOptions::Checks::None:
    return "none";
  }
  return "trap";
}

/// The largest local, in bytes, that `weavec-cc` zero-initialises.
static constexpr unsigned ZeroInitMaxSize = 4096;

/// RFC 0035 §1: zero-initialised locals, Clang's `array-bounds` checks
/// reported through the runtime, location tracking for the reports, and
/// the guard pass. Returns the ledger the pass fills.
static std::shared_ptr<EnforcementLedger>
configureEnforcement(clang::CompilerInstance &compiler,
                     const DriverOptions &weavec,
                     std::shared_ptr<const UnsafeRegions> unsafe) {
  clang::LangOptions &lang = compiler.getLangOpts();
  clang::CodeGenOptions &codegen = compiler.getCodeGenOpts();
  if (weavec.zeroInitialises()) {
    lang.setTrivialAutoVarInit(
        clang::LangOptions::TrivialAutoVarInitKind::Zero);
    // A local larger than this is not zero-initialised (unless the command
    // line set its own limit): clearing a buffer of hundreds of KiB on each
    // iteration of a loop that declares it cost a test suite 14x.
    if (lang.TrivialAutoVarInitMaxSize == 0)
      lang.TrivialAutoVarInitMaxSize = ZeroInitMaxSize;
  }
  // Lifetime markers at every optimisation level, for the scopes of
  // section 3.3 (Clang emits them at -O0 for this flag only).
  codegen.SanitizeAddressUseAfterScope = true;
  // A command line that asks for a sanitizer keeps its own handlers.
  if (lang.Sanitize.empty()) {
    lang.Sanitize.set(clang::SanitizerKind::ArrayBounds, true);
    if (weavec.checks == DriverOptions::Checks::Report)
      codegen.SanitizeRecover.set(clang::SanitizerKind::ArrayBounds, true);
  }
  if (codegen.getDebugInfo() == llvm::codegenoptions::NoDebugInfo) {
    codegen.setDebugInfo(llvm::codegenoptions::LocTrackingOnly);
    codegen.DebugColumnInfo = true;
  }
  auto ledger = std::make_shared<EnforcementLedger>();
  registerGuardPasses(codegen, GuardOptions{.mode = guardMode(weavec.checks),
                                            .unsafe = std::move(unsafe),
                                            .ledger = ledger});
  return ledger;
}

/// Writes the unit's enforcement ledger and prints its summary line.
static bool emitEnforcementLedger(const clang::CompilerInstance &compiler,
                                  const DriverOptions &weavec,
                                  const EnforcementLedger &ledger) {
  const clang::FrontendOptions &frontend = compiler.getFrontendOpts();
  EnforcementUnit unit;
  if (!frontend.Inputs.empty() && frontend.Inputs.front().isFile())
    unit.source = frontend.Inputs.front().getFile().str();
  if (frontend.OutputFile != "-")
    unit.object = frontend.OutputFile;
  unit.target = compiler.getTargetOpts().Triple;
  unit.checks = checksName(weavec.checks).str();
  unit.zeroInit = weavec.zeroInitialises();
  bool ok = true;
  if (!weavec.ledger.empty()) {
    std::string error;
    if (!writeEnforcementLedger(ledger, unit, weavec.ledger, error)) {
      llvm::errs() << "weavec-cc: error: " << error << '\n';
      ok = false;
    }
  }
  if (weavec.summary.value_or(!weavec.ledger.empty()))
    llvm::errs() << enforcementSummary(ledger, unit.source) << '\n';
  return ok;
}

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

  // A guard's fast path is a shadow load and a branch to its partial-
  // granule check. AArch64's conditional compares fold the two into one
  // dependent chain on every access, which costs more than the branch it
  // saves (an interpreter's loop: 2.68x against 2.35x); a later -mllvm option
  // of the user's wins.
  std::vector<std::string> &llvmArgs = compiler->getFrontendOpts().LLVMArgs;
  if (weavec.enabled && weavec.checks != DriverOptions::Checks::None &&
      llvm::Triple(compiler->getTargetOpts().Triple).isAArch64())
    llvmArgs.insert(llvmArgs.begin(), "-aarch64-enable-ccmp=false");

  if (!compiler->getFrontendOpts().LLVMArgs.empty()) {
    std::vector<const char *> args{"weavec-cc (LLVM option parsing)"};
    for (const std::string &arg : compiler->getFrontendOpts().LLVMArgs)
      args.push_back(arg.c_str());
    llvm::cl::ParseCommandLineOptions(static_cast<int>(args.size()),
                                      args.data());
  }

  // WeaveC guards C. Anything else is Clang's alone.
  const clang::LangOptions &lang = compiler->getLangOpts();
  if (!weavec.enabled || lang.CPlusPlus || lang.ObjC || lang.OpenCL ||
      lang.CUDA)
    return clang::ExecuteCompilerInvocation(compiler.get()) ? 0 : 1;

  std::unique_ptr<clang::FrontendAction> inner =
      clang::CreateFrontendAction(*compiler);
  if (!inner)
    return 1;
  if (inner->usesPreprocessorOnly())
    return compiler->ExecuteAction(*inner) ? 0 : 1;

  const bool codegen =
      isCodeGenAction(compiler->getFrontendOpts().ProgramAction);
  std::shared_ptr<UnsafeRegions> unsafe;
  std::shared_ptr<EnforcementLedger> ledger;
  if (weavec.enforces() && codegen) {
    unsafe = std::make_shared<UnsafeRegions>();
    ledger = configureEnforcement(*compiler, weavec, unsafe);
  }
  if (driver != nullptr && driver->stats)
    weavec.stats = driver->stats;
  std::optional<FrontendOptions> analysis;
  if (weavec.diagnose)
    analysis = analysisOptions(weavec);

  WeaveCWrapperAction action(std::move(inner), unsafe, std::move(analysis));
  success = compiler->ExecuteAction(action);
  if (success && ledger)
    success = emitEnforcementLedger(*compiler, weavec, *ledger);
  const bool statsOK =
      driver != nullptr ||
      writeAnalysisStats(weavec.analysisStatsPath, weavec.stats.get());
  return success && statsOK ? 0 : 1;
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

/// The ledgers one invocation writes: one per compile job of a C source.
static std::size_t countLedgers(const clang::driver::Compilation &compilation) {
  std::size_t count = 0;
  for (const clang::driver::Command &job : compilation.getJobs()) {
    const clang::driver::Action::ActionClass kind = job.getSource().getKind();
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

/// RFC 0033 §10: the driver names `<weavec-cc>/../lib/libLTO.dylib`, which
/// a WeaveC install does not have. The link then uses the libLTO of the
/// LLVM WeaveC was built with, which reads the bitcode its own compiler
/// writes (the system linker's own cannot read a newer LLVM's), or, when
/// that LLVM has none, the linker's own.
static void replaceMissingLtoLibrary(clang::driver::Compilation &compilation) {
  std::string ownLto;
  if (const std::string clang = getClangExecutable(); !clang.empty()) {
    llvm::SmallString<256> path(
        llvm::sys::path::parent_path(llvm::sys::path::parent_path(clang)));
    llvm::sys::path::append(path, "lib", "libLTO.dylib");
    if (llvm::sys::fs::exists(path))
      ownLto = path.str().str();
  }
  for (clang::driver::Command &job : compilation.getJobs()) {
    if (job.getSource().getKind() != clang::driver::Action::LinkJobClass)
      continue;
    const llvm::opt::ArgStringList &old = job.getArguments();
    llvm::opt::ArgStringList args;
    bool changed = false;
    for (std::size_t i = 0; i < old.size(); ++i) {
      if (llvm::StringRef(old[i]) == "-lto_library" && i + 1 < old.size() &&
          !llvm::sys::fs::exists(old[i + 1])) {
        changed = true;
        ++i;
        if (!ownLto.empty()) {
          args.push_back("-lto_library");
          args.push_back(compilation.getArgs().MakeArgString(ownLto));
        }
        continue;
      }
      args.push_back(old[i]);
    }
    if (changed)
      job.replaceArguments(args);
  }
}

/// RFC 0035 §7: why the build cannot use the runtime, from the command line
/// alone; empty when it can.
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

/// RFC 0035 §7: adds the runtime archives to every link job, before the
/// first library on its line (a linker takes a symbol from the first
/// library that defines it, and scans an archive only for what is undefined
/// so far):
///
///   libweavec_alloc.a  the image's allocator, forced in by `-u malloc`
///   libweavec_rt.a     the shadow, the guards' slow paths and the reports
///
/// The archives are host code, so they are added only when the link targets
/// the host.
static bool addRuntimeLibraries(clang::driver::Compilation &compilation,
                                const char *argv0, void *mainAddress) {
  const llvm::Triple target = compilation.getDefaultToolChain().getTriple();
  const llvm::Triple host(llvm::sys::getProcessTriple());
  if (target.getArch() != host.getArch() || target.getOS() != host.getOS())
    return true;
  const std::string runtime =
      findRuntimeLibrary(argv0, mainAddress, "libweavec_rt.a");
  const std::string allocator =
      findRuntimeLibrary(argv0, mainAddress, "libweavec_alloc.a");
  if (runtime.empty() || allocator.empty()) {
    llvm::errs() << "weavec-cc: error: cannot find the WeaveC runtime "
                    "(libweavec_rt.a, libweavec_alloc.a), which an enforcing "
                    "link carries (-fweavec-checks=none builds without it)\n";
    return false;
  }
  const auto owned = [&compilation](const std::string &text) {
    return compilation.getArgs().MakeArgString(text);
  };
  for (clang::driver::Command &job : compilation.getJobs()) {
    if (job.getSource().getKind() != clang::driver::Action::LinkJobClass)
      continue;
    const llvm::opt::ArgStringList &old = job.getArguments();
    llvm::opt::ArgStringList added;
    added.push_back("-u");
    added.push_back(target.isOSDarwin() ? "_malloc" : "malloc");
    added.push_back(owned(allocator));
    added.push_back(owned(runtime));
    if (target.isOSLinux() && !target.isAndroid())
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

/// The first input of a link, an object or a member of an archive, that
/// defines the allocator itself; empty when none does. The allocator is what
/// libweavec_alloc.a defines strongly: a second definition of one of those
/// could not be linked with it.
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

/// A program that defines the allocator keeps it. Takes libweavec_alloc.a
/// (and the `-u` that forces it) off the link line, and says once what that
/// means.
static void dropAllocatorIfDefined(clang::driver::Command &link) {
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
  llvm::errs() << "weavec-cc: note: '" << definer
               << "' defines the allocator, so the WeaveC runtime's is not "
                  "linked: the heap is untracked, guards pass on it and "
                  "releases are not validated\n";
}

int runCc1(llvm::ArrayRef<const char *> argv, const char *argv0) {
  return runCc1Job(argv, argv0);
}

int runDriver(llvm::ArrayRef<const char *> argv, void *mainAddress) {
  // RFC 0033 §10: printed after everything Clang prints for `--version`.
  struct VersionTrailer {
    bool print = false;
    VersionTrailer() = default;
    VersionTrailer(const VersionTrailer &) = delete;
    VersionTrailer &operator=(const VersionTrailer &) = delete;
    ~VersionTrailer() {
      if (print)
        printVersion(llvm::outs());
    }
  } versionTrailer;
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
    // RFC 0033 §10: Clang's version block first, so a configure script that
    // reads the first line takes the Clang path; WeaveC's own after it.
    if (text == "--version")
      versionTrailer.print = true;
    if (text == "-###")
      printJobsOnly = true;
    if (text.starts_with("-isysroot") || text.starts_with("--sysroot"))
      hasSysroot = true;
    if (looksLikeSource(text))
      hasSource = true;
    clangArgs.push_back(arg);
  }
  // Builds the runtime cannot serve are compiled without guards; the link
  // says so.
  std::string runtimeOff;
  if (weavec.enforces()) {
    runtimeOff = runtimeObstacle(clangArgs);
    if (!runtimeOff.empty()) {
      weavec.checks = DriverOptions::Checks::None;
      weavec.spellings.emplace_back("-fweavec-checks=none");
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

  // RFC 0034 §8: the driver's own diagnostics obey the -W options as
  // Clang's do (CMake's flag probes rely on
  // -Werror=unused-command-line-argument).
  const std::unique_ptr<clang::DiagnosticOptions> diagOptions =
      clang::CreateAndPopulateDiagOpts(clangArgs);
  auto *printer = new clang::TextDiagnosticPrinter(llvm::errs(), *diagOptions);
  printer->setPrefix("weavec-cc");
  clang::DiagnosticsEngine diags(
      llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>(), *diagOptions, printer);
  clang::ProcessWarningOptions(diags, *diagOptions,
                               *llvm::vfs::getRealFileSystem(),
                               /*ReportDiags=*/false);

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
  if (weavec.enforces() && !weavec.ledger.empty() &&
      !isLedgerDirectory(weavec.ledger)) {
    if (const std::size_t count = countLedgers(*compilation); count > 1) {
      llvm::errs() << "weavec-cc: error: '-fweavec-ledger=" << weavec.ledger
                   << "' would receive " << count
                   << " ledgers; name a directory (ending in '/') to get one "
                      "ledger per unit\n";
      return 1;
    }
  }
  if (weavec.enforces() &&
      !addRuntimeLibraries(*compilation, argv[0], mainAddress))
    return 1;
  const bool links = llvm::any_of(
      compilation->getJobs(), [](const clang::driver::Command &job) {
        return job.getSource().getKind() == clang::driver::Action::LinkJobClass;
      });
  if (!runtimeOff.empty() && links)
    llvm::errs() << "weavec-cc: note: building without the WeaveC runtime ("
                 << runtimeOff << "): memory accesses are not guarded\n";
  replaceMissingLtoLibrary(*compilation);
  if (printJobsOnly) {
    compilation->getJobs().Print(llvm::errs(), "\n", /*Quote=*/true);
    return 0;
  }

  // Every cc1 job runs here, whatever their number, so that a WeaveC flag
  // means the same thing in a one-step build as in a `-c` build.
  for (clang::driver::Command &job : compilation->getJobs()) {
    if (!job.getArguments().empty() &&
        llvm::StringRef(job.getArguments().front()) == "-cc1")
      job.InProcess = true;
  }

  int status = 0;
  for (clang::driver::Command &job : compilation->getJobs()) {
    // The objects exist now: a program-defined allocator is visible.
    if (job.getSource().getKind() == clang::driver::Action::LinkJobClass &&
        weavec.enforces())
      dropAllocatorIfDefined(job);
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
  const bool statsOK =
      writeAnalysisStats(weavec.analysisStatsPath, weavec.stats.get());
  return statsOK ? status : 1;
}

} // namespace weavec::frontend
