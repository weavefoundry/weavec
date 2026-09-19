//===- main.cpp - The weavec command-line tool ----------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `weavec` runs WeaveC's analyses over C sources using libTooling:
//
//   weavec file.c -- -I include -DFOO
//   weavec -p build/ file.c            (using a compile_commands.json)
//   weavec --whole-program -p build/   (every file of the database, as one
//                                       program; RFC 0005)
//
// Warning control follows the compiler's spelling: -Wno-weavec-<id>,
// -Werror=weavec-<id>, -Wno-error=weavec. RFC 0030 §16: the ledger
// (--ledger, --ledger-format), the require level (--require), the budget
// (--budget) and --no-zero-init model a `weavec-cc` build with the default
// checks, and the summary line is always printed. The drop-in compiler
// driver is `weavec-cc`.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/AttributeReader.h"
#include "weavec/Analysis/KindInference.h"
#include "weavec/Analysis/KindTable.h"
#include "weavec/Analysis/SlotCollector.h"
#include "weavec/Config/Version.h"
#include "weavec/Core/FnSlots.h"
#include "weavec/Core/Ledger.h"
#include "weavec/Core/LibrarySpec.h"
#include "weavec/Frontend/AnalysisStats.h"
#include "weavec/Frontend/DiagnosticControl.h"
#include "weavec/Frontend/FrontendAction.h"
#include "weavec/Frontend/LedgerOutput.h"
#include "weavec/Frontend/LedgerWriter.h"
#include "weavec/Frontend/LinkStep.h"
#include "weavec/Frontend/ProgramAnalysis.h"
#include "weavec/Frontend/RecordPayload.h"
#include "weavec/Frontend/ResourceDir.h"
#include "weavec/Frontend/UnitRecord.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace cl = llvm::cl;

// Command-line options are registered through global objects by design.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
cl::OptionCategory weavecCategory("weavec options");

cl::opt<std::string>
    analysisStatsPath("analysis-stats",
                      cl::desc("Write analysis work statistics JSON"),
                      cl::cat(weavecCategory));

cl::opt<std::string> ledgerPath(
    "ledger",
    cl::desc("Write the ledger of every outcome: to this file, or, for a "
             "directory (a value ending in '/'), one <source>.ledger.json "
             "per source"),
    cl::value_desc("path"), cl::cat(weavecCategory));

cl::opt<weavec::frontend::LedgerFormat>
    ledgerFormat("ledger-format", cl::desc("The ledger's format"),
                 cl::values(clEnumValN(weavec::frontend::LedgerFormat::Json,
                                       "json", "weavec-ledger JSON (default)"),
                            clEnumValN(weavec::frontend::LedgerFormat::Sarif,
                                       "sarif", "SARIF 2.1.0")),
                 cl::init(weavec::frontend::LedgerFormat::Json),
                 cl::cat(weavecCategory));

cl::opt<weavec::core::RequireLevel> requireLevel(
    "require",
    cl::desc("Make facets that are not proven errors (unresolved-operation, "
             "unchecked-operation)"),
    cl::values(clEnumValN(weavec::core::RequireLevel::None, "none",
                          "no requirement (default)"),
               clEnumValN(weavec::core::RequireLevel::Checked, "checked",
                          "every facet proven or checkable"),
               clEnumValN(weavec::core::RequireLevel::Proven, "proven",
                          "every facet proven")),
    cl::init(weavec::core::RequireLevel::None), cl::cat(weavecCategory));

cl::opt<std::uint64_t>
    budget("budget",
           cl::desc("Block transfers per function before its analysis stops "
                    "(0: unlimited)"),
           cl::init(weavec::core::DefaultBudget), cl::cat(weavecCategory));

cl::opt<bool> noZeroInit("no-zero-init",
                         cl::desc("Model a build without zero-initialisation "
                                  "(-fno-weavec-zero-init)"),
                         cl::init(false), cl::cat(weavecCategory));

cl::opt<bool> dumpAnalysis(
    "dump-analysis",
    cl::desc("Print the inferred places, lifetimes, exit state and summary "
             "of every analysed function to stdout (debugging aid; format "
             "unstable)"),
    cl::init(false), cl::cat(weavecCategory));

cl::opt<bool> dumpKinds(
    "dump-kinds",
    cl::desc("Print each unit's pointer kinds (RFC 0030 §7), must-access "
             "requirements, store groups, field candidates and "
             "function-pointer slots to stdout instead of analysing "
             "(debugging aid; format unstable)"),
    cl::init(false), cl::cat(weavecCategory));

cl::opt<std::string> dumpRecord(
    "dump-record",
    cl::desc("Print the WeaveC unit record at <path> (an <object>.weavec that "
             "weavec-cc wrote) as JSON and exit; a stale record is an error "
             "that says why (debugging aid)"),
    cl::value_desc("path"), cl::cat(weavecCategory));

cl::opt<bool> wholeProgram(
    "whole-program",
    cl::desc("Analyse the given sources (all sources of the compilation "
             "database if none are given) as one program, so calls into "
             "other files are checked against their definitions"),
    cl::init(false), cl::cat(weavecCategory));

// HelpMessage is a constant-initialised string literal, so the usual
// initialisation-order concern does not apply (this is the libTooling idiom).
// NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init)
cl::extrahelp commonHelp(clang::tooling::CommonOptionsParser::HelpMessage);
cl::extrahelp moreHelp(
    "\nWarning control: -Wno-weavec-<id> disables a warning, -Wweavec-<id>\n"
    "re-enables it, -Werror=weavec[-<id>] and -Wno-error=weavec[-<id>] change\n"
    "severities. Errors cannot be disabled, only lowered to warnings.\n"
    "-Wweavec-allocation-failure enables the one id that is off by default.\n"
    "\nWeaveC brings inferred ownership and borrowing to existing C code.\n"
    "See https://github.com/weavefoundry/weavec for documentation.\n");

// Any symbol inside this executable works for locating it on disk.
int mainExecutableAnchor = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

} // namespace

namespace {

/// `--dump-kinds`: the declared and inferred kinds and the slots of a unit,
/// before any engine runs (RFC 0030 §7, §9.3).
class DumpKindsConsumer : public clang::ASTConsumer {
public:
  // NOLINTNEXTLINE(readability-identifier-naming): ASTConsumer's hook
  void HandleTranslationUnit(clang::ASTContext &context) override {
    const weavec::core::LibrarySpec &library =
        weavec::core::LibrarySpec::shipped();
    weavec::analysis::KindTable kinds =
        weavec::analysis::AttributeReader(context, library).read();
    const weavec::analysis::SlotCollection slots =
        weavec::analysis::SlotCollector(context, library).collect();
    const weavec::core::SlotSolution solution = slots.solve();
    weavec::analysis::KindInferenceOptions options;
    options.slots = &slots;
    options.slotSolution = &solution;
    const weavec::analysis::KindInferenceResult inferred =
        weavec::analysis::KindInference(context, library, options).infer(kinds);
    llvm::outs() << "unit '" << slots.unit() << "'\n";
    weavec::analysis::dumpKinds(kinds, inferred, context, llvm::outs());
    weavec::analysis::dumpSlots(slots, solution, context, llvm::outs());
  }
};

class DumpKindsAction : public clang::ASTFrontendAction {
protected:
  // NOLINTNEXTLINE(readability-identifier-naming): FrontendAction's hook
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance & /*compiler*/,
                    llvm::StringRef /*file*/) override {
    return std::make_unique<DumpKindsConsumer>();
  }
};

} // namespace

/// `--dump-record`: the record at `path` as JSON, or why it is stale.
static int printRecord(llvm::StringRef path) {
  std::string reason;
  const std::optional<weavec::frontend::record::UnitRecord> record =
      weavec::frontend::record::readRecord(path, reason);
  if (!record) {
    llvm::errs() << "weavec: error: '" << path << "' is a stale WeaveC record ("
                 << reason << ")\n";
    return 1;
  }
  // The payload is read as the link step reads it, so what it rejects is
  // stale here too.
  if (!weavec::frontend::record::payloadFromJson(
          record->payload, record->header.source, reason)) {
    llvm::errs() << "weavec: error: '" << path << "' is a stale WeaveC record ("
                 << reason << ")\n";
    return 1;
  }
  llvm::outs() << weavec::frontend::record::renderRecord(*record);
  return 0;
}

static std::string currentDirectory() {
  llvm::SmallString<256> cwd;
  if (llvm::sys::fs::current_path(cwd))
    return {};
  return cwd.str().str();
}

/// RFC 0030 §13.2 in `weavec --whole-program`: the declarations verified
/// against their definitions and the program ledger, from the units' last
/// runs. False after an error.
static bool
finishProgram(const weavec::frontend::ProgramAnalysis &program,
              const clang::tooling::CompilationDatabase &compilations,
              const std::vector<std::string> &sources,
              const weavec::frontend::FrontendOptions &options) {
  namespace frontend = weavec::frontend;
  const std::string cwd = currentDirectory();
  std::vector<frontend::ProgramMember> members;
  std::vector<const weavec::core::Ledger *> runs;
  std::string programName = "program";
  for (std::size_t i = 0; i < program.unitCount(); ++i) {
    const weavec::analysis::UnitExports *exports = program.exportsOf(i);
    if (exports == nullptr)
      continue;
    frontend::ProgramMember member;
    member.source = program.unitName(i);
    if (i < sources.size()) {
      const auto commands = compilations.getCompileCommands(sources[i]);
      if (!commands.empty())
        member.cwd = commands.front().Directory;
    }
    member.payload.exports = *exports;
    if (const auto *facts = program.interfaceOf(i))
      member.payload.facts = *facts;
    const weavec::core::Ledger *ledger = program.ledgerOf(i);
    if (ledger != nullptr && !ledger->units.empty()) {
      member.payload.sites = frontend::record::siteRows(ledger->units.front());
      member.payload.a5 = ledger->units.front().a5;
      if (llvm::any_of(ledger->units.front().functions,
                       [](const weavec::core::FunctionLedger &function) {
                         return function.name == "main";
                       }))
        programName = llvm::sys::path::stem(member.source).str();
    }
    member.payload.reported = program.reportedOf(i);
    members.push_back(std::move(member));
    runs.push_back(ledger);
  }
  const frontend::DeclarationCheck declarations =
      frontend::verifyDeclarations(members, cwd);
  frontend::LinkDiagnosticPrinter printer("weavec");
  frontend::FilteringSink sink(printer, options.control);
  for (const weavec::core::Diagnostic &diagnostic : declarations.diagnostics)
    sink.report(diagnostic);
  weavec::core::Ledger ledger = frontend::composeProgramLedger(
      frontend::ProgramLedgerInput{.members = members,
                                   .runs = runs,
                                   .copyRecordRows = false,
                                   .declarations = &declarations,
                                   .shape = {},
                                   .linkDiagnostics = {},
                                   .cwd = cwd});
  frontend::applyDiagnosticControl(ledger, options.control);
  frontend::LedgerOutputOptions output = options.ledgerOutput;
  output.path = ledgerPath.getValue();
  std::string error;
  if (!frontend::emitProgramLedger(ledger, programName, cwd, options.config,
                                   output, llvm::errs(), &error)) {
    llvm::errs() << "weavec: error: cannot write the program ledger: " << error
                 << '\n';
    return false;
  }
  return sink.errors() == 0;
}

static void printVersion(llvm::raw_ostream &os) {
  os << "weavec version " << WEAVEC_VERSION_STRING;
  os << " (" << WEAVEC_GIT_REVISION;
  if (WEAVEC_GIT_DIRTY)
    os << "-dirty";
  os << ")\n";
  os << "  built with LLVM " << WEAVEC_LLVM_VERSION_STRING << "\n";
}

/// Removes WeaveC's `-W` flags (which `llvm::cl` would reject) from the
/// tool's own arguments, before `--`, and applies them to `control`. False
/// after printing an error for a malformed one.
static bool extractWarningFlags(int &argc, const char **argv,
                                weavec::frontend::DiagnosticControl &control) {
  int kept = 1;
  bool passthrough = false;
  for (int i = 1; i < argc; ++i) {
    const llvm::StringRef arg = argv[i];
    if (!passthrough && arg == "--")
      passthrough = true;
    std::string error;
    if (!passthrough && control.parse(arg, error)) {
      if (!error.empty()) {
        llvm::errs() << "weavec: error: " << error << '\n';
        return false;
      }
      continue;
    }
    argv[kept++] = argv[i];
  }
  argc = kept;
  return true;
}

/// An option `CommonOptionsParser` registers itself (`-p`, `--extra-arg`,
/// `--extra-arg-before`) and keeps no accessor for.
template <typename Option>
static const Option *commonOption(llvm::StringRef name) {
  const auto &options = cl::getRegisteredOptions();
  const auto found = options.find(name);
  // The parser registers each of these names with exactly this type.
  return found == options.end() ? nullptr
                                : static_cast<const Option *>(found->second);
}

/// RFC 0030 §16: the database of `-p` for a run that names no source.
/// `CommonOptionsParser` loads none then (`cl::ZeroOrMore`), so it is loaded
/// here, with `--extra-arg-before` and `--extra-arg` applied as the parser
/// applies them. Null, with `error` set, when there is none.
static std::unique_ptr<clang::tooling::CompilationDatabase>
loadBuildDatabase(std::string &error) {
  const auto *buildPath = commonOption<cl::opt<std::string>>("p");
  if (buildPath == nullptr || buildPath->getValue().empty()) {
    error = "no compilation database with sources; give -p <build-dir> or "
            "list the files";
    return nullptr;
  }
  std::string message;
  std::unique_ptr<clang::tooling::CompilationDatabase> database =
      clang::tooling::CompilationDatabase::autoDetectFromDirectory(
          buildPath->getValue(), message);
  if (!database) {
    // As for `-p` with a source, the directory's parents are searched too.
    error = "no compilation database in '" + buildPath->getValue() +
            "' or any parent directory";
    return nullptr;
  }
  auto adjusted =
      std::make_unique<clang::tooling::ArgumentsAdjustingCompilations>(
          std::move(database));
  const auto extraArgs = [](llvm::StringRef name) {
    std::vector<std::string> args;
    if (const auto *list = commonOption<cl::list<std::string>>(name))
      args.assign(list->begin(), list->end());
    return args;
  };
  adjusted->appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      extraArgs("extra-arg-before"),
      clang::tooling::ArgumentInsertPosition::BEGIN));
  adjusted->appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      extraArgs("extra-arg"), clang::tooling::ArgumentInsertPosition::END));
  return adjusted;
}

/// Whether a compile command asks for `-fdiagnostics-format=sarif` (the last
/// format given wins, as in Clang).
static bool asksForSarif(const std::vector<std::string> &command) {
  llvm::StringRef format;
  for (std::size_t i = 0; i < command.size(); ++i) {
    llvm::StringRef arg = command[i];
    if (arg.consume_front("-fdiagnostics-format="))
      format = arg;
    else if (arg == "-fdiagnostics-format" && i + 1 < command.size())
      format = command[++i];
  }
  return format.equals_insensitive("sarif");
}

/// The argument adjusters every parse gets: the annotation header and the
/// `__WEAVEC__` feature macro, Clang's builtin headers, and a default SDK on
/// Apple platforms.
static std::vector<clang::tooling::ArgumentsAdjuster>
makeAdjusters(const char *argv0) {
  std::vector<clang::tooling::ArgumentsAdjuster> adjusters;

  const std::string resourceInclude =
      weavec::frontend::findResourceIncludeDir(argv0, &mainExecutableAnchor);
  if (!resourceInclude.empty()) {
    adjusters.push_back(clang::tooling::getInsertArgumentAdjuster(
        {"-isystem", resourceInclude},
        clang::tooling::ArgumentInsertPosition::BEGIN));
  }
  adjusters.push_back(clang::tooling::getInsertArgumentAdjuster(
      "-D__WEAVEC__=1", clang::tooling::ArgumentInsertPosition::BEGIN));

  // Use the builtin headers of the Clang we were built against. An explicit
  // -resource-dir on the command line still takes precedence (it comes later).
  const std::string clangResourceDir = weavec::frontend::getClangResourceDir();
  if (!clangResourceDir.empty()) {
    adjusters.push_back(clang::tooling::getInsertArgumentAdjuster(
        ("-resource-dir=" + clangResourceDir).c_str(),
        clang::tooling::ArgumentInsertPosition::BEGIN));
  }

  // On Apple platforms the driver library has no default SDK; mirror what the
  // `clang` binary does unless the user already specified a sysroot.
  const std::string sysroot = weavec::frontend::getDefaultSysroot();
  if (!sysroot.empty()) {
    adjusters.emplace_back([sysroot](
                               const clang::tooling::CommandLineArguments &args,
                               llvm::StringRef /*filename*/) {
      const bool hasSysroot = llvm::any_of(args, [](const std::string &arg) {
        return llvm::StringRef(arg).starts_with("-isysroot") ||
               llvm::StringRef(arg).starts_with("--sysroot");
      });
      if (hasSysroot)
        return args;
      clang::tooling::CommandLineArguments adjusted(args);
      adjusted.insert(adjusted.begin() + 1, {"-isysroot", sysroot});
      return adjusted;
    });
  }
  return adjusters;
}

int main(int argc, const char **argv) {
  llvm::InitLLVM init(argc, argv);
  cl::SetVersionPrinter(printVersion);
  cl::HideUnrelatedOptions(weavecCategory);

  weavec::frontend::DiagnosticControl control;
  if (!extractWarningFlags(argc, argv, control))
    return 1;

  // With `--`, the parser's database is the fixed one that follows it.
  const bool fixedDatabase =
      std::find(argv, argv + argc, llvm::StringRef("--")) != argv + argc;
  auto expectedParser = clang::tooling::CommonOptionsParser::create(
      argc, argv, weavecCategory, cl::ZeroOrMore,
      "weavec: memory-safety analysis for C");
  if (!expectedParser) {
    llvm::errs() << llvm::toString(expectedParser.takeError());
    return 1;
  }
  clang::tooling::CommonOptionsParser &parser = *expectedParser;
  if (!dumpRecord.empty())
    return printRecord(dumpRecord);

  // Without a source the parser loads no database (and has none to give
  // unless `--` built the fixed one): `--whole-program -p <dir>` loads it
  // here and analyses every file it lists.
  std::vector<std::string> sources = parser.getSourcePathList();
  std::unique_ptr<clang::tooling::CompilationDatabase> buildDatabase;
  if (sources.empty()) {
    if (!wholeProgram) {
      llvm::errs() << "weavec: error: no input files\n";
      return 1;
    }
    std::string error = "no compilation database with sources; give -p "
                        "<build-dir> or list the files";
    if (!fixedDatabase)
      buildDatabase = loadBuildDatabase(error);
    if (buildDatabase)
      sources = buildDatabase->getAllFiles();
    if (sources.empty()) {
      if (buildDatabase)
        error = "the compilation database lists no files";
      llvm::errs() << "weavec: error: " << error << '\n';
      return 1;
    }
  }
  const clang::tooling::CompilationDatabase &compilations =
      buildDatabase ? *buildDatabase : parser.getCompilations();

  // RFC 0030 §16: the tool's runs build their SourceManager before Clang
  // attaches a SARIF document writer to its printer, so Clang's SARIF
  // output would crash them. WeaveC's own SARIF is the ledger's.
  for (const std::string &source : sources) {
    for (const clang::tooling::CompileCommand &command :
         compilations.getCompileCommands(source)) {
      if (asksForSarif(command.CommandLine)) {
        llvm::errs() << "weavec: error: -fdiagnostics-format=sarif is not "
                        "supported; write the ledger as SARIF with "
                        "--ledger=<path> --ledger-format=sarif\n";
        return 1;
      }
    }
  }

  if (analysisStatsPath.getNumOccurrences() && analysisStatsPath.empty()) {
    llvm::errs() << "weavec: error: analysis statistics require a path\n";
    return 1;
  }
  // §16: one file receives one ledger; a directory one per source.
  if (!wholeProgram && sources.size() > 1 && !ledgerPath.empty() &&
      !weavec::frontend::isLedgerDirectory(ledgerPath)) {
    llvm::errs() << "weavec: error: '--ledger=" << ledgerPath
                 << "' would receive " << sources.size()
                 << " ledgers; name a directory (ending in '/') to get one "
                    "ledger per source\n";
    return 1;
  }
  weavec::core::AnalysisStats stats;
  weavec::frontend::FrontendOptions options;
  options.analysis.stats = analysisStatsPath.empty() ? nullptr : &stats;
  options.analysisStatsPath = analysisStatsPath.getValue();
  if (dumpAnalysis)
    options.analysis.dumpStream = &llvm::outs();
  options.control = control;
  // §16: the ledger models a `weavec-cc` build with the default checks, and
  // the summary line, always printed, says they are not enforced.
  options.config = weavec::core::LedgerConfig{
      .checks = weavec::core::ChecksMode::Trap,
      .zeroInit = !noZeroInit,
      .require = requireLevel,
      .budget = budget,
  };
  // With --whole-program, --ledger names the program ledger, which the
  // program analysis writes (`emitProgramLedger`), not a ledger per unit.
  options.ledgerOutput = weavec::frontend::LedgerOutputOptions{
      .path = wholeProgram ? std::string() : ledgerPath.getValue(),
      .format = ledgerFormat,
      .summary = true,
      .checksEnforced = false,
  };

  const std::vector<clang::tooling::ArgumentsAdjuster> adjusters =
      makeAdjusters(argv[0]);

  if (wholeProgram) {
    weavec::frontend::ProgramAnalysis program(options);
    // RFC 0030 §13.2: `weavec --whole-program` runs the link step's checks.
    program.collectInterfaces(true);
    for (const std::string &source : sources) {
      program.addUnit(
          std::make_unique<weavec::frontend::CompilationDatabaseUnit>(
              compilations, source, adjusters));
    }
    const weavec::frontend::ProgramAnalysis::Result result = program.run();
    for (const std::string &name : result.failed)
      llvm::errs() << "weavec: error: cannot analyse '" << name << "'\n";
    for (const std::vector<std::string> &component : result.nonConverging) {
      llvm::errs() << "weavec: error: whole-program analysis of ";
      llvm::interleaveComma(component, llvm::errs(), [](const std::string &n) {
        llvm::errs() << '\'' << n << '\'';
      });
      llvm::errs() << " did not converge\n";
    }
    const bool finished =
        finishProgram(program, compilations, sources, options);
    const bool statsOK = weavec::frontend::writeAnalysisStats(
        analysisStatsPath, options.analysis.stats);
    return result.ok() && finished && statsOK ? 0 : 1;
  }

  clang::tooling::ClangTool tool(compilations, sources);
  for (const clang::tooling::ArgumentsAdjuster &adjuster : adjusters)
    tool.appendArgumentsAdjuster(adjuster);
  if (dumpKinds)
    return tool.run(
        clang::tooling::newFrontendActionFactory<DumpKindsAction>().get());
  const int status =
      tool.run(weavec::frontend::createWeaveCActionFactory(options).get());
  const bool statsOK = weavec::frontend::writeAnalysisStats(
      analysisStatsPath, options.analysis.stats);
  return status == 0 && statsOK ? 0 : 1;
}
