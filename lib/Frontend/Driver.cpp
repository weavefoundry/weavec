//===- Driver.cpp - weavec-cc, the compiler driver ------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Driver.h"

#include "weavec/Config/Version.h"
#include "weavec/Frontend/EnforcementLedger.h"
#include "weavec/Frontend/GuardPass.h"
#include "weavec/Frontend/ResourceDir.h"
#include "weavec/Frontend/UnsafeRegions.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/Sanitizers.h"
#include "clang/CodeGen/CodeGenAction.h"
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
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/PreprocessorOptions.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
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

#ifndef WEAVEC_RUNTIME_ABI
#define WEAVEC_RUNTIME_ABI 0
#endif

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
  static constexpr std::array<llvm::StringLiteral, 2> Names{"-fweavec-checks",
                                                            "-fweavec-ledger"};
  const auto [flag, value] = arg.split('=');
  if (!llvm::is_contained(Names, flag))
    return false;
  if (flag == arg || value.empty()) {
    error = "missing value for '" + flag.str() + "'";
    return true;
  }
  if (flag == "-fweavec-checks") {
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
  } else {
    options.ledger = value.str();
  }
  options.spellings.push_back(arg.str());
  return true;
}

bool DriverOptions::consume(llvm::StringRef arg, std::string &error) {
  if (consumeValueFlag(*this, arg, error))
    return true;
  if (arg == "-fno-weavec-rpath") {
    rpath = std::string();
    return true;
  }
  if (llvm::StringRef dir = arg; dir.consume_front("-fweavec-rpath")) {
    if (!dir.consume_front("=") || dir.empty())
      error = "missing value for '-fweavec-rpath'";
    else
      rpath = dir.str();
    return true;
  }
  bool value = true;
  llvm::StringRef name = arg;
  if (name.consume_front("-fno-")) {
    value = false;
  } else if (!name.consume_front("-f")) {
    return false;
  }
  if (name == "weavec") {
    enabled = value;
  } else if (name == "weavec-asan-compat") {
    asanCompat = value;
  } else if (name == "weavec-colors") {
    colors = value;
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
  return R"(WeaveC flags of weavec-cc (RFC 0037); every other flag is Clang's.

  -fweavec, -fno-weavec
      Guard memory accesses (the default), or compile as plain Clang.
  -fweavec-checks=trap|report|verify|none
      What a failed guard does: trap (the default); report it once per site
      and continue; trap, with every guard a local proof removed kept as a
      monitor (verify); or nothing is guarded (none).
  -fweavec-zero-init, -fno-weavec-zero-init
      Zero-initialize locals (default: on unless -fweavec-checks=none).
  -fweavec-ledger=<path>
      Write the unit's enforcement ledger: what was guarded, proven or left
      unguarded, access by access. A directory (a value ending in '/')
      receives <object>.ledger.json per unit; a file receives one ledger.
  -fweavec-summary, -fno-weavec-summary
      Print the summary line on stderr (default: when a ledger is written).
  -fweavec-asan-compat, -fno-weavec-asan-compat
      Compile as AddressSanitizer would for the source: __SANITIZE_ADDRESS__
      and __has_feature(address_sanitizer) (default: on).
  -fweavec-colors, -fno-weavec-colors
      Check an access computed from a pointer against the color of the
      pointer's object, so that a jump into a neighbor traps (default: on).
  -fweavec-rpath=<dir>, -fno-weavec-rpath
      Where a linked image finds the runtime's library (libweavec_rt):
      <dir> ($ORIGIN, @loader_path or an installation path) instead of the
      toolchain's runtime directory, or no rpath at all.
)";
}

//===----------------------------------------------------------------------===//
// The -cc1 step
//===----------------------------------------------------------------------===//

/// RFC 0038 §6: Clang's action for a `-cc1` job, of those a C build asks
/// for: code generation, preprocessing, syntax checks and precompiled
/// headers. (Clang's own selection, in clangFrontendTool, links the static
/// analyzer and its solver into the driver.) Null, with an error, for
/// another.
static std::unique_ptr<clang::FrontendAction>
createAction(clang::CompilerInstance &compiler) {
  switch (compiler.getFrontendOpts().ProgramAction) {
  case clang::frontend::EmitAssembly:
    return std::make_unique<clang::EmitAssemblyAction>();
  case clang::frontend::EmitBC:
    return std::make_unique<clang::EmitBCAction>();
  case clang::frontend::EmitLLVM:
    return std::make_unique<clang::EmitLLVMAction>();
  case clang::frontend::EmitLLVMOnly:
    return std::make_unique<clang::EmitLLVMOnlyAction>();
  case clang::frontend::EmitCodeGenOnly:
    return std::make_unique<clang::EmitCodeGenOnlyAction>();
  case clang::frontend::EmitObj:
    return std::make_unique<clang::EmitObjAction>();
  case clang::frontend::GeneratePCH:
    return std::make_unique<clang::GeneratePCHAction>();
  case clang::frontend::InitOnly:
    return std::make_unique<clang::InitOnlyAction>();
  case clang::frontend::ParseSyntaxOnly:
    return std::make_unique<clang::SyntaxOnlyAction>();
  case clang::frontend::PrintPreprocessedInput:
    return std::make_unique<clang::PrintPreprocessedAction>();
  case clang::frontend::RunPreprocessorOnly:
    return std::make_unique<clang::PreprocessOnlyAction>();
  case clang::frontend::DumpTokens:
    return std::make_unique<clang::DumpTokensAction>();
  case clang::frontend::ASTDump:
    return std::make_unique<clang::ASTDumpAction>();
  case clang::frontend::DumpCompilerOptions:
    return std::make_unique<clang::DumpCompilerOptionsAction>();
  default:
    llvm::errs() << "weavec-cc: error: this job's action (static analysis, "
                    "modules, rewriting) is not in weavec-cc\n";
    return nullptr;
  }
}

/// An object inside this executable, for finding it (its address only).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static int executableAnchor = 0;

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

/// RFC 0036 §1: the unit is parsed with AddressSanitizer in its language
/// options (its macros, `__has_feature(address_sanitizer)`); at the end of
/// the unit, before the code generator's backend reads them, this takes it
/// out again, so that ASan's own instrumentation never runs.
class AsanOptionsReset final : public clang::ASTConsumer {
public:
  explicit AsanOptionsReset(clang::LangOptions &lang) : lang(lang) {}
  void HandleTranslationUnit(clang::ASTContext & /*context*/) override {
    lang.Sanitize.set(clang::SanitizerKind::Address, false);
  }

private:
  clang::LangOptions &lang;
};

/// Clang's action for the job with the unsafe-region collector running
/// beside its code generator.
class WeaveCWrapperAction final : public clang::WrapperFrontendAction {
public:
  WeaveCWrapperAction(std::unique_ptr<clang::FrontendAction> wrapped,
                      std::shared_ptr<UnsafeRegions> unsafe, bool asanCompat)
      : WrapperFrontendAction(std::move(wrapped)), unsafe(std::move(unsafe)),
        asanCompat(asanCompat) {}

protected:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &compiler,
                    llvm::StringRef inFile) override {
    std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
    if (unsafe)
      consumers.push_back(createUnsafeRegionCollector(compiler, unsafe));
    if (asanCompat)
      consumers.push_back(
          std::make_unique<AsanOptionsReset>(compiler.getLangOpts()));
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
  bool asanCompat;
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

/// The largest local, in bytes, that `weavec-cc` zero-initializes.
static constexpr unsigned ZeroInitMaxSize = 4096;

/// RFC 0035 §1: zero-initialized locals, Clang's `array-bounds` checks
/// reported through the runtime, location tracking for the reports, and
/// the guard pass. Returns the ledger the pass fills.
static std::shared_ptr<EnforcementLedger>
configureEnforcement(clang::CompilerInstance &compiler,
                     const DriverOptions &weavec,
                     std::shared_ptr<const UnsafeRegions> unsafe) {
  clang::LangOptions &lang = compiler.getLangOpts();
  clang::CodeGenOptions &codegen = compiler.getCodeGenOpts();
  if (weavec.zeroInitializes()) {
    lang.setTrivialAutoVarInit(
        clang::LangOptions::TrivialAutoVarInitKind::Zero);
    // A local larger than this is not zero-initialized (unless the command
    // line set its own limit): clearing a buffer of hundreds of KiB on each
    // iteration of a loop that declares it cost a test suite 14x.
    if (lang.TrivialAutoVarInitMaxSize == 0)
      lang.TrivialAutoVarInitMaxSize = ZeroInitMaxSize;
  }
  // Lifetime markers at every optimization level, for the scopes of
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
                                            .ledger = ledger,
                                            .colors = weavec.colors});
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
  unit.zeroInit = weavec.zeroInitializes();
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

/// RFC 0036 §5: ASan's macros for the source (`__SANITIZE_ADDRESS__`,
/// `__has_feature(address_sanitizer)`) unless a sanitizer of the command
/// line already gives them; whether it did.
static bool applyAsanCompat(clang::CompilerInstance &compiler,
                            const DriverOptions &weavec) {
  if (!weavec.enforces() || !weavec.asanCompat ||
      compiler.getLangOpts().Sanitize.hasOneOf(
          clang::SanitizerKind::Address | clang::SanitizerKind::KernelAddress |
          clang::SanitizerKind::HWAddress))
    return false;
  compiler.getLangOpts().Sanitize.set(clang::SanitizerKind::Address, true);
  // Clang predefines _FORTIFY_SOURCE=0 for ASan on Darwin (ASan intercepts
  // the C library there); WeaveC checks the fortified forms (§5.5), so the
  // SDK's own default applies unless the command line chose a level.
  clang::PreprocessorOptions &preprocessor = compiler.getPreprocessorOpts();
  const bool chosen = llvm::any_of(preprocessor.Macros, [](const auto &macro) {
    return llvm::StringRef(macro.first).starts_with("_FORTIFY_SOURCE");
  });
  if (llvm::Triple(compiler.getTargetOpts().Triple).isOSDarwin() && !chosen)
    preprocessor.addMacroUndef("_FORTIFY_SOURCE");
  return true;
}

static int runCc1Job(llvm::ArrayRef<const char *> argv, const char *argv0) {
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

  // The driver passes -resource-dir; a hand-written cc1 line may not: the
  // toolchain's, beside this executable (RFC 0038 §6).
  clang::HeaderSearchOptions &headers = compiler->getHeaderSearchOpts();
  if (headers.UseBuiltinIncludes && headers.ResourceDir.empty())
    headers.ResourceDir = clang::GetResourcesPath(
        llvm::sys::fs::getMainExecutable(argv0, &executableAnchor));

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
  std::unique_ptr<clang::FrontendAction> inner = createAction(*compiler);
  if (!inner)
    return 1;
  if (!weavec.enabled || lang.CPlusPlus || lang.ObjC || lang.OpenCL ||
      lang.CUDA)
    return compiler->ExecuteAction(*inner) ? 0 : 1;
  if (inner->usesPreprocessorOnly()) {
    // The same macros as the compile that follows (ccache's preprocessor
    // mode, Autoconf's CPP checks).
    (void)applyAsanCompat(*compiler, weavec);
    return compiler->ExecuteAction(*inner) ? 0 : 1;
  }

  const bool codegen =
      isCodeGenAction(compiler->getFrontendOpts().ProgramAction);
  std::shared_ptr<UnsafeRegions> unsafe;
  std::shared_ptr<EnforcementLedger> ledger;
  if (weavec.enforces() && codegen) {
    unsafe = std::make_shared<UnsafeRegions>();
    ledger = configureEnforcement(*compiler, weavec, unsafe);
  }
  // Only code generation takes ASan out at the end of the unit: a
  // precompiled header or module keeps it in its options, as the compiles
  // that read it have it.
  const bool asanCompat = applyAsanCompat(*compiler, weavec) && codegen;
  WeaveCWrapperAction action(std::move(inner), unsafe, asanCompat);
  success = compiler->ExecuteAction(action);
  if (success && ledger)
    success = emitEnforcementLedger(*compiler, weavec, *ledger);
  return success ? 0 : 1;
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

/// Whether `arg` is an assembly source (`.s`, `.S`, `.sx`), which Clang's
/// integrated assembler (`-cc1as`, a tool Clang does not ship as a library)
/// would assemble.
static bool looksLikeAssembly(llvm::StringRef arg) {
  if (arg.starts_with("-"))
    return false;
  const llvm::StringRef ext = llvm::sys::path::extension(arg);
  return ext == ".s" || ext == ".S" || ext == ".sx";
}

static void printVersion(llvm::raw_ostream &os) {
  os << "weavec-cc version " << WEAVEC_VERSION_STRING << " ("
     << WEAVEC_GIT_REVISION;
  if (WEAVEC_GIT_DIRTY)
    os << "-dirty";
  os << ")\n  built with LLVM " << WEAVEC_LLVM_VERSION_STRING << "\n"
     << "  runtime ABI " << WEAVEC_RUNTIME_ABI << ", for "
     << llvm::sys::getProcessTriple() << "\n";
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
/// a WeaveC toolchain does not have: the link uses the linker's own.
static void replaceMissingLtoLibrary(clang::driver::Compilation &compilation) {
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

/// RFC 0036 §1: whether the build should define `_FORTIFY_SOURCE=3`: a glibc
/// target, an optimizing compile (glibc fortifies only then, and warns
/// otherwise), and a command line that does not define or undefine it.
static bool wantsFortification(llvm::ArrayRef<const char *> args) {
  std::string triple = llvm::sys::getDefaultTargetTriple();
  bool optimizes = false;
  for (std::size_t i = 1; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (arg.starts_with("-D_FORTIFY_SOURCE") ||
        arg.starts_with("-U_FORTIFY_SOURCE") ||
        ((arg == "-D" || arg == "-U") && i + 1 < args.size() &&
         llvm::StringRef(args[i + 1]).starts_with("_FORTIFY_SOURCE")))
      return false;
    if (arg.starts_with("-O"))
      optimizes = arg != "-O0";
    if (arg.consume_front("--target="))
      triple = arg.str();
    else if ((arg == "-target" || arg == "--target") && i + 1 < args.size())
      triple = args[++i];
  }
  const llvm::Triple target(llvm::Triple::normalize(triple));
  return optimizes && target.isOSLinux() && target.isGNUEnvironment();
}

/// Whether the command line links a static program (`-static`,
/// `-static-pie`).
static bool linksStatically(llvm::ArrayRef<const char *> args) {
  return llvm::any_of(args.drop_front(), [](const char *arg) {
    const llvm::StringRef text(arg);
    return text == "-static" || text == "-static-pie";
  });
}

/// RFC 0038 §1: adds the runtime's library to every link job, before the
/// first library on its line, so that the image depends on it before the C
/// library and the loader gives the process one runtime:
///
///   Darwin  <rt>/libweavec_rt.dylib, -rpath <rt>, <rt>/libweavec_rt.a
///   Linux   <rt>/libweavec_rt.so (soname libweavec_rt.so.<abi>), -rpath
///           <rt>, <rt>/libweavec_rt.a and -lpthread; a static program
///           takes libweavec_rt.a alone
///
/// `<rt>` is the toolchain's runtime directory, and the rpath what
/// -fweavec-rpath says. The library is host code, so it is added only when
/// the link targets the host.
static bool addRuntimeLibraries(clang::driver::Compilation &compilation,
                                const char *argv0, void *mainAddress,
                                const DriverOptions &weavec, bool linkStatic) {
  const llvm::Triple target = compilation.getDefaultToolChain().getTriple();
  const llvm::Triple host(llvm::sys::getProcessTriple());
  if (target.getArch() != host.getArch() || target.getOS() != host.getOS())
    return true;
  const bool archive = linkStatic && !target.isOSDarwin();
  const char *name = "libweavec_rt.so";
  if (target.isOSDarwin())
    name = "libweavec_rt.dylib";
  else if (archive)
    name = "libweavec_rt.a";
  const std::string runtime = findRuntimeLibrary(argv0, mainAddress, name);
  if (runtime.empty()) {
    llvm::errs() << "weavec-cc: error: cannot find the WeaveC runtime (" << name
                 << "), which an enforcing link carries "
                    "(-fweavec-checks=none builds without it)\n";
    return false;
  }
  const auto owned = [&compilation](const std::string &text) {
    return compilation.getArgs().MakeArgString(text);
  };
  const std::string rpath =
      weavec.rpath.value_or(llvm::sys::path::parent_path(runtime).str());
  // After the shared library, its archive: no member is taken, since the
  // library defines every entry point, but a build system that copies a C
  // compiler's implicit libraries into another language's link (CMake's
  // mixed C and C++ targets, linked by c++) finds it there, and that program
  // carries the runtime itself.
  const std::string fallback =
      archive ? std::string()
              : findRuntimeLibrary(argv0, mainAddress, "libweavec_rt.a");
  for (clang::driver::Command &job : compilation.getJobs()) {
    if (job.getSource().getKind() != clang::driver::Action::LinkJobClass)
      continue;
    const llvm::opt::ArgStringList &old = job.getArguments();
    // A relocatable link (`-r`) makes an object, not an image.
    if (llvm::is_contained(old, llvm::StringRef("-r")))
      continue;
    llvm::opt::ArgStringList added;
    added.push_back(owned(runtime));
    if (!archive && !rpath.empty()) {
      added.push_back("-rpath");
      added.push_back(owned(rpath));
    }
    if (!fallback.empty())
      added.push_back(owned(fallback));
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

/// A program that defines the allocator keeps it: the loader binds
/// `malloc` to the program's definition before the runtime's. Says so once,
/// naming the first input of the link, an object or a member of an archive,
/// that defines `malloc` strongly.
static void noteProgramAllocator(const clang::driver::Command &link,
                                 const llvm::Triple &target) {
  const llvm::StringRef malloc = target.isOSDarwin() ? "_malloc" : "malloc";
  for (const clang::driver::InputInfo &input : link.getInputInfos()) {
    if (!input.isFilename())
      continue;
    const std::string path = input.getFilename();
    const bool defines =
        anyRelocatableObject(path, [&](const llvm::object::SymbolicFile &file) {
          return anyGlobalDefinition(file,
                                     [&](const std::string &name, bool weak) {
                                       return !weak && name == malloc;
                                     });
        });
    if (defines) {
      llvm::errs() << "weavec-cc: note: '" << path
                   << "' defines the allocator, which serves the program "
                      "instead of the WeaveC runtime's: the heap is "
                      "untracked, guards pass on it and releases are not "
                      "validated\n";
      return;
    }
  }
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
    if (mode.starts_with("-cc1")) {
      llvm::errs() << "weavec-cc: error: '" << mode
                   << "' is Clang's tool, which weavec-cc does not include; "
                      "assembly sources are assembled by the system "
                      "assembler\n";
      return 1;
    }
  }

  const std::string executable =
      llvm::sys::fs::getMainExecutable(argv[0], mainAddress);

  DriverOptions weavec;
  std::vector<const char *> clangArgs;
  // Storage for arguments synthesized here; a deque keeps the pointers
  // handed to `clangArgs` valid as it grows.
  std::deque<std::string> owned;
  bool printJobsOnly = false;
  bool hasSysroot = false;
  bool hasSource = false;
  // RFC 0038 §6: an assembly source, or a compile that keeps its assembly,
  // goes through the system assembler (`-cc1as` is not in Clang's
  // libraries).
  bool assembles = false;
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
    if (looksLikeAssembly(text) || text.starts_with("-save-temps") ||
        (llvm::StringRef(clangArgs.back()) == "-x" &&
         text.starts_with("assembler")) ||
        text.starts_with("-xassembler"))
      assembles = true;
    // A source file, or `-` (standard input) other than `-o`'s operand.
    if (looksLikeSource(text) ||
        (text == "-" && llvm::StringRef(clangArgs.back()) != "-o"))
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
  if (assembles && llvm::none_of(clangArgs, [](const char *arg) {
        return llvm::StringRef(arg).ends_with("integrated-as");
      }))
    add("-fno-integrated-as");
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
      if (weavec.enforces() && wantsFortification(clangArgs))
        add("-D_FORTIFY_SOURCE=3");
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
  const auto cc1 = [](llvm::SmallVectorImpl<const char *> &args) {
    return runCc1Job(llvm::ArrayRef(args).drop_front(2), args[0]);
  };
  driver.CC1Main = cc1;

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
      !addRuntimeLibraries(*compilation, argv[0], mainAddress, weavec,
                           linksStatically(clangArgs)))
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
  for (const clang::driver::Command &job : compilation->getJobs()) {
    // The objects exist now: a program-defined allocator is visible.
    if (job.getSource().getKind() == clang::driver::Action::LinkJobClass &&
        weavec.enforces())
      noteProgramAllocator(job, compilation->getDefaultToolChain().getTriple());
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
  return status;
}

} // namespace weavec::frontend
