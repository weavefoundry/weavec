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
// -Werror=weavec-<id>, -Wno-error=weavec. The drop-in compiler driver is
// `weavec-cc`.
//
//===----------------------------------------------------------------------===//

#include "weavec/Config/Version.h"
#include "weavec/Frontend/DiagnosticControl.h"
#include "weavec/Frontend/FrontendAction.h"
#include "weavec/Frontend/ProgramAnalysis.h"
#include "weavec/Frontend/ResourceDir.h"

#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <vector>

namespace {

namespace cl = llvm::cl;

// Command-line options are registered through global objects by design.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
cl::OptionCategory weavecCategory("weavec options");

cl::opt<bool> reportUnannotated(
    "report-unannotated",
    cl::desc("Warn about unannotated pointer parameters and results of "
             "exported functions, offering the inferred annotation as a "
             "fix-it, and about calls to unannotated functions from system "
             "headers"),
    cl::init(false), cl::cat(weavecCategory));

cl::opt<bool> strictExterns(
    "strict-externs",
    cl::desc("Treat calls into unchecked code (no definition, annotation or "
             "library summary) as raw operations: an error outside "
             "WEAVEC_UNSAFE regions, and their pointer results are raw"),
    cl::init(false), cl::cat(weavecCategory));

cl::opt<bool> exclusiveBorrows(
    "exclusive-borrows",
    cl::desc("Enforce Rust's exclusivity rule in full: a second mutable "
             "borrow, a shared borrow of a mutably borrowed object or a write "
             "to a borrowed object is a conflicting-borrow. By default only "
             "freeing, moving or reallocating a borrowed object is"),
    cl::init(false), cl::cat(weavecCategory));

cl::opt<bool> analyzeHeaders(
    "analyze-headers",
    cl::desc("Also analyse function definitions found in included headers"),
    cl::init(false), cl::cat(weavecCategory));

cl::opt<bool> dumpAnalysis(
    "dump-analysis",
    cl::desc("Print the inferred places, lifetimes, exit state and summary "
             "of every analysed function to stdout (debugging aid; format "
             "unstable)"),
    cl::init(false), cl::cat(weavecCategory));

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
    "\nWeaveC brings inferred ownership and borrowing to existing C code.\n"
    "See https://github.com/weavefoundry/weavec for documentation.\n");

// Any symbol inside this executable works for locating it on disk.
int mainExecutableAnchor = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

} // namespace

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

  auto expectedParser = clang::tooling::CommonOptionsParser::create(
      argc, argv, weavecCategory, cl::ZeroOrMore,
      "weavec: memory-safety analysis for C");
  if (!expectedParser) {
    llvm::errs() << llvm::toString(expectedParser.takeError());
    return 1;
  }
  clang::tooling::CommonOptionsParser &parser = *expectedParser;
  const clang::tooling::CompilationDatabase &compilations =
      parser.getCompilations();

  std::vector<std::string> sources = parser.getSourcePathList();
  if (sources.empty()) {
    if (!wholeProgram) {
      llvm::errs() << "weavec: error: no input files\n";
      return 1;
    }
    sources = compilations.getAllFiles();
    if (sources.empty()) {
      llvm::errs() << "weavec: error: no compilation database with sources; "
                      "give -p <build-dir> or list the files\n";
      return 1;
    }
  }

  weavec::frontend::FrontendOptions options;
  options.analysis.reportUnannotated = reportUnannotated;
  options.analysis.strictExterns = strictExterns;
  options.analysis.exclusiveBorrows = exclusiveBorrows;
  if (dumpAnalysis)
    options.analysis.dumpStream = &llvm::outs();
  options.mainFileOnly = !analyzeHeaders;
  options.control = control;

  const std::vector<clang::tooling::ArgumentsAdjuster> adjusters =
      makeAdjusters(argv[0]);

  if (wholeProgram) {
    weavec::frontend::ProgramAnalysis program(options);
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
    return result.ok() ? 0 : 1;
  }

  clang::tooling::ClangTool tool(compilations, sources);
  for (const clang::tooling::ArgumentsAdjuster &adjuster : adjusters)
    tool.appendArgumentsAdjuster(adjuster);
  return tool.run(weavec::frontend::createWeaveCActionFactory(options).get());
}
