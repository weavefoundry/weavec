//===- EnforcementLedger.h - What the guard pass decided --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0035 §9. The guard pass records each access it collected, before it
// removes any guard: where it is, what it does, and whether it is guarded,
// proven (its guard removed by a rule of §6) or unguarded. `weavec-cc`
// writes the rows of a unit as JSON (`weavec-ledger` version 3) under
// `-fweavec-ledger` and prints the summary line under `-fweavec-summary`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_ENFORCEMENTLEDGER_H
#define WEAVEC_FRONTEND_ENFORCEMENTLEDGER_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace weavec::frontend {

/// The version of the `weavec-ledger` JSON this writes.
inline constexpr unsigned EnforcementLedgerVersion = 3;

/// What the pass decided for one access.
struct LedgerRow {
  enum class Outcome : std::uint8_t { Guarded, Proven, Unguarded };
  std::string function;
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
  /// `load`, `store`, `rmw`, `copy`, `set`, `lane`, `call:<name>`.
  std::string operation;
  /// The bytes it accesses; 0 when they are computed at run time.
  std::uint64_t bytes = 0;
  Outcome outcome = Outcome::Guarded;
  /// `access`, `range`, `string`, `checked-call`, `loop-range`; `in-bounds`,
  /// `dominated`, `merged`; `unsafe`.
  std::string reason;
};

/// The rows of one unit, and their counts by outcome. The pass adds a row
/// for each access when it inserts the access's guard, and later says, for
/// each copy of that guard the optimiser left (inlining and unrolling copy
/// guards; redundancy elimination merges them), whether the copy stays or a
/// rule removed it; `finish` settles each row's outcome: guarded when a copy
/// stays, proven otherwise.
struct EnforcementLedger {
  std::vector<LedgerRow> rows;
  std::uint64_t guarded = 0;
  std::uint64_t proven = 0;
  std::uint64_t unguarded = 0;

  /// Adds a row; its index names it.
  unsigned add(LedgerRow row);
  /// A copy of row `index`'s guard stays, or was removed with `reason`.
  void copy(unsigned index, bool stays, llvm::StringRef reason);
  /// Settles the outcomes of the rows whose guards were inserted, and
  /// counts them.
  void finish();

  [[nodiscard]] std::uint64_t accesses() const {
    return guarded + proven + unguarded;
  }

private:
  struct Copies {
    unsigned stay = 0;
    unsigned removed = 0;
    std::string reason;
  };
  std::vector<Copies> copies;
};

/// The ledger spelling of an outcome.
[[nodiscard]] const char *outcomeName(LedgerRow::Outcome outcome);

/// Where a unit's ledger comes from.
struct EnforcementUnit {
  std::string source;
  std::string object;
  std::string target;
  /// `trap`, `report` or `verify`.
  std::string checks;
  bool zeroInit = true;
};

/// A `-fweavec-ledger` value names a directory when it ends in a path
/// separator or names an existing directory.
[[nodiscard]] bool isLedgerDirectory(llvm::StringRef value);

/// Writes the unit's ledger where `path` says: the file, or
/// `<object>.ledger.json` in the directory, through a temporary file renamed
/// into place. False with `error` set when it cannot.
bool writeEnforcementLedger(const EnforcementLedger &ledger,
                            const EnforcementUnit &unit, llvm::StringRef path,
                            std::string &error);

/// The summary line: `weavec: <source>: <n> accesses: <p> proven, <g>
/// guarded, <n> unguarded`.
[[nodiscard]] std::string enforcementSummary(const EnforcementLedger &ledger,
                                             llvm::StringRef source);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_ENFORCEMENTLEDGER_H
