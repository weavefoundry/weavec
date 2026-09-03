//===- Driver.h - weavec-cc, the compiler driver ---------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `weavec-cc` is Clang's driver with WeaveC inside (RFC 0005, *`weavec-cc`*):
//
//   weavec-cc -c foo.c -o foo.o      compile foo.c, analyse it alone, write
//                                    foo.o and foo.o.weavec
//   weavec-cc foo.o bar.o -o prog    analyse the program the sidecars
//                                    describe, then link
//
// The `-fweavec-*` and `-W*weavec*` flags are WeaveC's; everything else is
// Clang's. Each `-cc1` job runs in this process with WeaveC's AST consumer
// multiplexed beside Clang's, so one parse serves both code generation and
// analysis; a WeaveC error fails the compile like any other error.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DRIVER_H
#define WEAVEC_FRONTEND_DRIVER_H

#include "weavec/Frontend/DiagnosticControl.h"
#include "weavec/Frontend/FrontendAction.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace weavec::frontend {

/// WeaveC's own command-line flags, split from Clang's.
struct DriverOptions {
  /// `-fweavec` / `-fno-weavec`: analyse at all.
  bool enabled = true;
  /// `-fweavec-strict`: `--strict-externs`.
  bool strict = false;
  /// `-fweavec-exclusive-borrows`: `--exclusive-borrows` (RFC 0006).
  bool exclusiveBorrows = false;
  /// `-fweavec-report-unannotated`.
  bool reportUnannotated = false;
  /// `-fweavec-analyze-headers`.
  bool analyzeHeaders = false;
  /// `-fweavec-dump-analysis`.
  bool dumpAnalysis = false;
  /// `-fweavec-link` / `-fno-weavec-link`: run the whole-program step
  /// before linking.
  bool link = true;
  DiagnosticControl control;
  /// The flags consumed, in order, for forwarding to `-cc1` jobs.
  std::vector<std::string> spellings;

  /// True if `arg` is one of WeaveC's flags; it has then been applied and
  /// recorded. `error` is set (and true returned) for a malformed one.
  bool consume(llvm::StringRef arg, std::string &error);

  /// The frontend options these flags ask for.
  [[nodiscard]] FrontendOptions toFrontendOptions() const;
};

/// Runs `weavec-cc` with the given command line (`argv[0]` included).
/// `mainAddress` is any address inside the executable, for locating it.
int runDriver(llvm::ArrayRef<const char *> argv, void *mainAddress);

/// Runs a `-cc1` command line (`argv` excludes `-cc1`): Clang's compiler
/// proper with WeaveC's consumer multiplexed in, writing the unit's sidecar
/// next to the output.
int runCc1(llvm::ArrayRef<const char *> argv, const char *argv0);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DRIVER_H
