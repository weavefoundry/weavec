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
//                                    foo.o and its unit record foo.o.weavec
//   weavec-cc foo.o bar.o -o prog    verify and analyse the program the
//                                    records describe (RFC 0030 §13.2),
//                                    then link
//
// The `-fweavec-*` and `-W*weavec*` flags are WeaveC's; everything else is
// Clang's. Each `-cc1` job runs in this process with WeaveC's AST consumer
// multiplexed beside Clang's, so one parse serves both code generation and
// analysis; a WeaveC error fails the compile like any other error.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DRIVER_H
#define WEAVEC_FRONTEND_DRIVER_H

#include "weavec/Core/Ledger.h"
#include "weavec/Frontend/DiagnosticControl.h"
#include "weavec/Frontend/FrontendAction.h"
#include "weavec/Frontend/LedgerWriter.h"
#include "weavec/Frontend/Prelude.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weavec::frontend {

/// WeaveC's own command-line flags, split from Clang's (RFC 0030 §16).
struct DriverOptions {
  std::string analysisStatsPath;
  std::shared_ptr<core::AnalysisStats> stats;
  /// `-fweavec` / `-fno-weavec`: analyse at all.
  bool enabled = true;
  /// `-fweavec-dump-analysis`.
  bool dumpAnalysis = false;
  /// `-fweavec-link` / `-fno-weavec-link`: run the whole-program step
  /// before linking.
  bool link = true;
  /// `-fweavec-checks=trap|report|verify|none` (§10.7).
  core::ChecksMode checks = core::ChecksMode::Trap;
  /// `-fweavec-zero-init` / `-fno-weavec-zero-init` (§11); unset means on.
  /// Zero-initialisation never applies with `-fweavec-checks=none`.
  std::optional<bool> zeroInit;
  /// `-fweavec-require=none|checked|proven` (§6.3).
  core::RequireLevel require = core::RequireLevel::None;
  /// `-fweavec-budget=<n>` (§5.5); 0 means unlimited.
  std::uint64_t budget = core::DefaultBudget;
  /// `-fweavec-ledger=<path>` (§16).
  std::string ledger;
  /// `-fweavec-ledger-format=json|sarif` (§12).
  LedgerFormat ledgerFormat = LedgerFormat::Json;
  /// `-fweavec-summary` / `-fno-weavec-summary`; unset means on exactly
  /// when a ledger is written (§12.4).
  std::optional<bool> summary;
  /// `-fweavec-print-prelude[=inline|out-of-line]` (§10.9): print the check
  /// prelude of the `-fweavec-checks` mode and exit. The out-of-line form
  /// is what the runtime build compiles into libweavec_chk.a.
  std::optional<PreludeForm> printPrelude;
  DiagnosticControl control;
  /// The flags consumed, in order, for forwarding to `-cc1` jobs.
  std::vector<std::string> spellings;

  /// True if `arg` is one of WeaveC's flags; it has then been applied and
  /// recorded. `error` is set (and true returned) for a malformed one.
  bool consume(llvm::StringRef arg, std::string &error);

  /// Whether the unit is zero-initialised: on unless
  /// `-fno-weavec-zero-init`, and never with `-fweavec-checks=none`.
  [[nodiscard]] bool zeroInitialises() const;

  /// The frontend options these flags ask for.
  [[nodiscard]] FrontendOptions toFrontendOptions() const;
};

/// The help text of WeaveC's `weavec-cc` flags (`weavec-cc --help-weavec`).
[[nodiscard]] llvm::StringRef driverFlagsHelp();

/// Runs `weavec-cc` with the given command line (`argv[0]` included).
/// `mainAddress` is any address inside the executable, for locating it.
int runDriver(llvm::ArrayRef<const char *> argv, void *mainAddress);

/// Runs a `-cc1` command line (`argv` excludes `-cc1`): Clang's compiler
/// proper with WeaveC's consumer multiplexed in, writing the unit record
/// (RFC 0030 §13.1) next to the output.
int runCc1(llvm::ArrayRef<const char *> argv, const char *argv0);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DRIVER_H
