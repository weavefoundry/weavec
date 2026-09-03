//===- main.cpp - The weavec-cc compiler driver ---------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `weavec-cc` is a drop-in C compiler (RFC 0005): Clang's driver with
// WeaveC's analysis in every compile and a whole-program check before every
// link.
//
//   weavec-cc -c foo.c -o foo.o
//   weavec-cc foo.o bar.o -o prog
//   CC=weavec-cc make
//
// WeaveC's flags: -fweavec / -fno-weavec, -fweavec-strict,
// -fweavec-exclusive-borrows, -fweavec-report-unannotated,
// -fweavec-analyze-headers,
// -fweavec-dump-analysis, -fno-weavec-link, -W[no-]weavec-<id>,
// -W[no-]error=weavec[-<id>]. Everything else is Clang's.
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/Driver.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/InitLLVM.h"

#include <cstddef>

namespace {
// Any symbol inside this executable works for locating it on disk.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
int mainExecutableAnchor = 0;
} // namespace

int main(int argc, const char **argv) {
  llvm::InitLLVM init(argc, argv);
  return weavec::frontend::runDriver(
      llvm::ArrayRef(argv, static_cast<std::size_t>(argc)),
      &mainExecutableAnchor);
}
