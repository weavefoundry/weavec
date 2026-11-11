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
// writes the rows of a unit as JSON (`weavec-ledger` version 4: RFC 0039
// §8 adds each row's `display`, its function's demangled name) under
// `-fweavec-ledger` and prints the summary line under `-fweavec-summary`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_FRONTEND_ENFORCEMENTLEDGER_H
#define WEAVEC_FRONTEND_ENFORCEMENTLEDGER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace weavec::frontend {

/// The version of the `weavec-ledger` JSON this writes.
inline constexpr unsigned EnforcementLedgerVersion = 5;

/// What the pass decided for one access.
struct LedgerRow {
  enum class Outcome : std::uint8_t { Guarded, Proven, Unguarded };
  /// The function's symbol (a C++ function's mangled name).
  std::string function;
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
  /// `load`, `store`, `rmw`, `copy`, `set`, `lane`, `call:<name>`.
  std::string operation;
  /// The bytes it accesses; 0 when they are computed at run time.
  std::uint64_t bytes = 0;
  Outcome outcome = Outcome::Guarded;
  /// `access`, `range`, `string`, `checked-call`, `loop-range`,
  /// `typed-unbounded`; `in-bounds`, `dominated`, `merged`, `safe`,
  /// `typed`; `unsafe`.
  std::string reason;
};

/// RFC 0042 §10: a function the unit defines, and its tier: `typed`,
/// `safe`, `guarded` or `unsafe`.
struct LedgerFunction {
  std::string function;
  std::string tier;
  std::string file;
  unsigned line = 0;
};

/// The rows of one unit, and their counts by outcome. The pass adds a row
/// for each access when it inserts the access's guard, and later says, for
/// each copy of that guard the optimizer left (inlining and unrolling copy
/// guards; redundancy elimination merges them), whether the copy stays or a
/// rule removed it; `finish` settles each row's outcome: guarded when a copy
/// stays, proven otherwise.
struct EnforcementLedger {
  std::vector<LedgerRow> rows;
  std::vector<LedgerFunction> functions;
  std::uint64_t guarded = 0;
  std::uint64_t proven = 0;
  std::uint64_t unguarded = 0;

  /// Adds a row; its index names it.
  unsigned add(LedgerRow row);
  /// A copy of row `index`'s guard stays, or was removed with `reason`.
  void copy(unsigned index, bool stays, llvm::StringRef reason);
  /// RFC 0042 §10: row `index`'s guard stays for `reason`.
  void setReason(unsigned index, llvm::StringRef reason) {
    if (index < rows.size())
      rows[index].reason = reason.str();
  }
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
/// guarded, <n> unguarded`, and in a unit with typed functions `; <t> of
/// <f> functions typed`.
[[nodiscard]] std::string enforcementSummary(const EnforcementLedger &ledger,
                                             llvm::StringRef source);

/// RFC 0042 §10: `weavec-cc --weavec-audit [--format=json] <ledger>...`:
/// what a program's typed functions trust, from its ledgers (files, or
/// directories searched for `*.ledger.json`). The exit status is 0, or 1
/// when a ledger cannot be read.
int runAudit(llvm::ArrayRef<const char *> operands);

} // namespace weavec::frontend

#endif // WEAVEC_FRONTEND_ENFORCEMENTLEDGER_H
