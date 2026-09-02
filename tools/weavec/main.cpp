//===- main.cpp - The weavec command-line tool ----------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `weavec` currently runs WeaveC's analyses over C sources using libTooling:
//
//   weavec file.c -- -I include -DFOO
//   weavec -p build/ file.c            (using a compile_commands.json)
//
// A drop-in compiler driver mode (acting as `cc`) is planned; see
// docs/roadmap.md.
//
//===----------------------------------------------------------------------===//

#include "weavec/Config/Version.h"
#include "weavec/Frontend/FrontendAction.h"
#include "weavec/Frontend/ResourceDir.h"

#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

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
    cl::desc("Treat calls to functions with no definition, annotation or "
             "library summary as errors instead of warnings"),
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

// HelpMessage is a constant-initialised string literal, so the usual
// initialisation-order concern does not apply (this is the libTooling idiom).
// NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init)
cl::extrahelp commonHelp(clang::tooling::CommonOptionsParser::HelpMessage);
cl::extrahelp moreHelp("\nWeaveC brings inferred ownership and borrowing to "
                       "existing C code.\nSee https://github.com/weavefoundry/"
                       "weavec for documentation.\n");

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

int main(int argc, const char **argv) {
  llvm::InitLLVM init(argc, argv);
  cl::SetVersionPrinter(printVersion);
  cl::HideUnrelatedOptions(weavecCategory);

  auto expectedParser = clang::tooling::CommonOptionsParser::create(
      argc, argv, weavecCategory, cl::OneOrMore,
      "weavec: memory-safety analysis for C");
  if (!expectedParser) {
    llvm::errs() << llvm::toString(expectedParser.takeError());
    return 1;
  }
  clang::tooling::CommonOptionsParser &parser = *expectedParser;

  clang::tooling::ClangTool tool(parser.getCompilations(),
                                 parser.getSourcePathList());

  // Make the annotation header (`#include <weavec.h>`) and the `__WEAVEC__`
  // feature macro available to every translation unit.
  const std::string resourceInclude =
      weavec::frontend::findResourceIncludeDir(argv[0], &mainExecutableAnchor);
  if (!resourceInclude.empty()) {
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        {"-isystem", resourceInclude},
        clang::tooling::ArgumentInsertPosition::BEGIN));
  }
  tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
      "-D__WEAVEC__=1", clang::tooling::ArgumentInsertPosition::BEGIN));

  // Use the builtin headers of the Clang we were built against. An explicit
  // -resource-dir on the command line still takes precedence (it comes later).
  const std::string clangResourceDir = weavec::frontend::getClangResourceDir();
  if (!clangResourceDir.empty()) {
    tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
        ("-resource-dir=" + clangResourceDir).c_str(),
        clang::tooling::ArgumentInsertPosition::BEGIN));
  }

  // On Apple platforms the driver library has no default SDK; mirror what the
  // `clang` binary does unless the user already specified a sysroot.
  const std::string sysroot = weavec::frontend::getDefaultSysroot();
  if (!sysroot.empty()) {
    tool.appendArgumentsAdjuster(
        [sysroot](const clang::tooling::CommandLineArguments &args,
                  llvm::StringRef /*filename*/) {
          const bool hasSysroot =
              llvm::any_of(args, [](const std::string &arg) {
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

  weavec::frontend::FrontendOptions options;
  options.analysis.reportUnannotated = reportUnannotated;
  options.analysis.strictExterns = strictExterns;
  if (dumpAnalysis)
    options.analysis.dumpStream = &llvm::outs();
  options.mainFileOnly = !analyzeHeaders;

  return tool.run(weavec::frontend::createWeaveCActionFactory(options).get());
}
