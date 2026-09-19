//===- main.cpp - weavec-prelude, the check prelude printer ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Prints the check prelude of RFC 0030 (section 10.2) for a mode and a
// target, the text `weavec-cc -fweavec-print-prelude` is to print:
//
//   weavec-prelude [--mode=trap|report|verify|none] [--target=<triple>]
//                  [--no-zero-init] [--no-verbose-trap] [--out-of-line]
//                  [-o <file>]
//
// The prelude tests compile its output, and the runtime build generates the
// helpers of libweavec_chk.a with `--out-of-line` (section 10.9).
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Prelude.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <string>
#include <system_error>

static int usage(llvm::StringRef problem) {
  llvm::errs() << "weavec-prelude: error: " << problem
               << "\nusage: weavec-prelude [--mode=trap|report|verify|none] "
                  "[--target=<triple>] [--no-zero-init] [--no-verbose-trap] "
                  "[--out-of-line] [-o <file>]\n";
  return 1;
}

int main(int argc, const char **argv) {
  const llvm::InitLLVM init(argc, argv);
  weavec::frontend::PreludeOptions options;
  std::string target = llvm::sys::getDefaultTargetTriple();
  std::string output = "-";
  for (int i = 1; i < argc; ++i) {
    llvm::StringRef arg(argv[i]);
    if (arg.consume_front("--mode=")) {
      const auto mode = weavec::frontend::parseCheckMode(arg);
      if (!mode)
        return usage("unknown mode '" + arg.str() + "'");
      options.mode = *mode;
    } else if (arg.consume_front("--target=")) {
      target = arg.str();
    } else if (arg == "--no-zero-init") {
      options.zeroInit = false;
    } else if (arg == "--no-verbose-trap") {
      options.verboseTrap = false;
    } else if (arg == "--out-of-line") {
      options.form = weavec::frontend::PreludeForm::OutOfLine;
    } else if (arg == "-o" && i + 1 < argc) {
      output = argv[++i];
    } else {
      return usage("unknown argument '" + arg.str() + "'");
    }
  }
  options.usableSize =
      weavec::frontend::usableSizeQueryFor(llvm::Triple(target));

  std::error_code error;
  llvm::raw_fd_ostream out(output, error);
  if (error)
    return usage("cannot write '" + output + "': " + error.message());
  out << weavec::frontend::buildCheckPrelude(options);
  return 0;
}
