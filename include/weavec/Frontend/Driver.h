//===- Driver.h - weavec-cc, the compiler driver ---------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `weavec-cc` is Clang's driver with WeaveC inside (RFC 0035 §1, §7, RFC 0036
// §1, §7):
//
//   weavec-cc -c foo.c -o foo.o      compile foo.c with every memory access
//                                    guarded (the guard pass), locals
//                                    zero-initialized and array indexes
//                                    bounds-checked
//   weavec-cc foo.o bar.o -o prog    link, with the WeaveC runtime
//
// The `-fweavec-*` flags are WeaveC's; everything else is Clang's. Each `-cc1`
// job runs in this process.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_DRIVER_H
#define WEAVEC_FRONTEND_DRIVER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
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
  /// `-f[no-]weavec-asan-compat` (RFC 0036 §1): parse with
  /// AddressSanitizer's language options, so the unit sees ASan's macros.
  bool asanCompat = true;
  /// `-f[no-]weavec-colors` (RFC 0037 §8): check arithmetic from a pointer
  /// against its object's color.
  bool colors = true;
  /// `-f[no-]weavec-safe` (RFC 0041 §10): every function the unit defines
  /// outside system headers is safe unless marked otherwise.
  bool safe = false;
  /// `-f[no-]weavec-typed` (RFC 0042 §1): the unit is typed; implies `safe`.
  bool typed = false;
  /// `-fweavec-safe-checks=bounds|full` (RFC 0041 §7): whether safe
  /// functions keep their guards beside their bounds checks.
  bool safeFull = false;
  /// `-fweavec-ledger=<path>` (RFC 0035 §9).
  std::string ledger;
  /// `-f[no-]weavec-summary`; unset means on exactly when a ledger is
  /// written.
  std::optional<bool> summary;
  /// `-fweavec-rpath=<dir>` / `-fno-weavec-rpath` (RFC 0038 §1): the rpath a
  /// link gets for the runtime's library; unset means the toolchain's
  /// runtime directory, empty means none. Link-only, never forwarded.
  std::optional<std::string> rpath;
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
  /// Whether locals are zero-initialized.
  [[nodiscard]] bool zeroInitializes() const {
    return enforces() && zeroInit.value_or(true);
  }
};

/// The help text of WeaveC's `weavec-cc` flags (`weavec-cc --help-weavec`).
[[nodiscard]] llvm::StringRef driverFlagsHelp();

/// Runs `weavec-cc` with the given command line (`commandLine[0]` included,
/// response files not yet expanded).
/// `mainAddress` is any address inside the executable, for locating it.
int runDriver(llvm::ArrayRef<const char *> commandLine, void *mainAddress);

/// Runs a `-cc1` command line (`argv` excludes `-cc1`): Clang's compiler
/// proper with WeaveC's guard passes and unsafe-region collector.
int runCc1(llvm::ArrayRef<const char *> argv, const char *argv0);

/// RFC 0040 §4.2: `weavec-cc --weavec-symbolize [<log>...]`, the runtime's
/// symbolizer (no operand) or a saved report's rewriter.
int runSymbolizer(llvm::ArrayRef<const char *> operands);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_DRIVER_H
