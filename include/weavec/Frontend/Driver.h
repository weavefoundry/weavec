//===- Driver.h - weavec-cc, the compiler driver ---------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `weavec-cc` is Clang's driver with WeaveC inside (RFC 0035 §1, §7):
//
//   weavec-cc -c foo.c -o foo.o      compile foo.c with every memory access
//                                    guarded (the guard pass), locals
//                                    zero-initialised and array indexes
//                                    bounds-checked
//   weavec-cc foo.o bar.o -o prog    link, with the WeaveC runtime
//
// The `-fweavec-*` and `-W*weavec*` flags are WeaveC's; everything else is
// Clang's. Each `-cc1` job runs in this process. `-fweavec-diagnose` adds the
// advisory analysis (RFC 0035 §8), whose findings are warnings.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DRIVER_H
#define WEAVEC_FRONTEND_DRIVER_H

#include "weavec/Core/AnalysisStats.h"
#include "weavec/Frontend/DiagnosticControl.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace weavec::frontend {

/// WeaveC's own command-line flags, split from Clang's.
struct DriverOptions {
  /// `-fweavec` / `-fno-weavec`: WeaveC at all.
  bool enabled = true;
  /// `-fweavec-checks=trap|report|verify|none` (RFC 0035 §7).
  enum class Checks : std::uint8_t { Trap, Report, Verify, None };
  Checks checks = Checks::Trap;
  /// `-f[no-]weavec-zero-init`; unset means on.
  std::optional<bool> zeroInit;
  /// `-fweavec-diagnose`: run the advisory analysis.
  bool diagnose = false;
  /// `-fweavec-dump-analysis`, with `-fweavec-diagnose`.
  bool dumpAnalysis = false;
  /// `-fweavec-budget=<n>`, `-fweavec-unit-budget=<n>`: the analysis's
  /// budgets (`-fweavec-diagnose`); unset means the defaults.
  std::optional<std::uint64_t> budget;
  std::optional<std::uint64_t> unitBudget;
  /// `-fweavec-ledger=<path>` (RFC 0035 §9).
  std::string ledger;
  /// `-f[no-]weavec-summary`; unset means on exactly when a ledger is
  /// written.
  std::optional<bool> summary;
  /// `-fweavec-analysis-stats=<path>`.
  std::string analysisStatsPath;
  std::shared_ptr<core::AnalysisStats> stats;
  /// The `-W` flags of the analysis's diagnostics; leaks are off unless
  /// asked for.
  DiagnosticControl control = [] {
    DiagnosticControl leakOff;
    leakOff.turnOffByDefault(core::diag::Leak);
    return leakOff;
  }();
  /// The flags consumed, in order, for forwarding to `-cc1` jobs.
  std::vector<std::string> spellings;

  /// True if `arg` is one of WeaveC's flags; it has then been applied and
  /// recorded. `error` is set (and true returned) for a malformed one.
  bool consume(llvm::StringRef arg, std::string &error);

  /// Whether the unit's accesses are guarded and the link carries the
  /// runtime.
  [[nodiscard]] bool enforces() const {
    return enabled && checks != Checks::None;
  }
  /// Whether locals are zero-initialised.
  [[nodiscard]] bool zeroInitialises() const {
    return enforces() && zeroInit.value_or(true);
  }
};

/// The help text of WeaveC's `weavec-cc` flags (`weavec-cc --help-weavec`).
[[nodiscard]] llvm::StringRef driverFlagsHelp();

/// Runs `weavec-cc` with the given command line (`argv[0]` included).
/// `mainAddress` is any address inside the executable, for locating it.
int runDriver(llvm::ArrayRef<const char *> argv, void *mainAddress);

/// Runs a `-cc1` command line (`argv` excludes `-cc1`): Clang's compiler
/// proper with WeaveC's guard pass, unsafe-region collector and, under
/// `-fweavec-diagnose`, analysis.
int runCc1(llvm::ArrayRef<const char *> argv, const char *argv0);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DRIVER_H
