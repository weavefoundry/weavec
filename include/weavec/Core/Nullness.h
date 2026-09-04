//===- Nullness.h - May-null / non-null facts per place --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `NullTracker` records what is known about the nullness of the pointer
// value each place holds (RFC 0008, *Nullness*): definitely null, possibly
// null, or known non-null. A place with no record has *unknown* nullness,
// which the checker trusts (a parameter, a loaded field, the result of code
// nobody here can see).
//
// The fact is a property of the value: it copies with the pointer and is
// dropped when the place is reassigned. `Null` and `MaybeNull` records carry
// where and why the value may be null, for the note on a `null-dereference`.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_NULLNESS_H
#define WEAVEC_CORE_NULLNESS_H

#include "weavec/Core/Place.h"
#include "weavec/Core/Scalar.h"
#include "weavec/Core/SourceLocation.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

enum class Nullness : std::uint8_t {
  /// The place holds a null pointer on every path reaching here.
  Null,
  /// The place holds a null pointer on some path reaching here.
  MaybeNull,
  /// The place holds a non-null pointer on every path reaching here.
  NonNull,
};

/// Why a place may be null.
enum class NullReason : std::uint8_t {
  /// A null constant was assigned.
  AssignedNull,
  /// The place received the result of a callee that may return null.
  CalleeResult,
  /// The place was stored to by a callee that may store null there.
  CalleeStore,
  /// The place was compared with null, and the path where it was null did
  /// not end (the two edges merged).
  Tested,
  /// The place's variable, parameter or field is declared `WEAVEC_NULLABLE`.
  Declared,
  /// The place was dereferenced, or passed to a callee that dereferences
  /// it, with nothing known: from there on it is non-null (the path would
  /// not have continued otherwise). Only ever `NonNull`.
  Dereferenced,
};

struct NullRecord {
  Nullness state = Nullness::MaybeNull;
  /// The assignment, call, test or declaration the fact comes from.
  SourceLocation location = {};
  NullReason reason = NullReason::AssignedNull;
  /// `CalleeResult` / `CalleeStore`: the callee's name as spelled in
  /// messages (`'malloc'`). Otherwise empty.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string detail = {};
  /// RFC 0009: the paths on which the place is null all satisfy the guard
  /// (the facts on the path that made it null). Trivial for `NonNull`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PlaceGuard guard = {};
  /// `MaybeNull` only: on every path where the guard does not hold the place
  /// is non-null, so refuting the guard makes the record `NonNull` rather
  /// than unknown (`p = NULL; if (n > 0) { p = malloc(n); if (!p) return; }
  /// if (n > 0) *p;`).
  bool otherwiseNonNull = false;

  [[nodiscard]] bool mayBeNull() const noexcept {
    return state != Nullness::NonNull;
  }

  friend bool operator==(const NullRecord &, const NullRecord &) = default;
};

/// Flow-insensitive record of nullness facts; the analysis driver clones and
/// joins trackers per CFG block, exactly as for `MoveTracker`.
class NullTracker {
public:
  /// `place` now has the nullness described by `record`, replacing any
  /// earlier fact.
  void set(PlaceId place, NullRecord record);

  /// The record for `place`, if it has one.
  [[nodiscard]] std::optional<NullRecord> recordOf(PlaceId place) const;
  /// The nullness of `place`, if known.
  [[nodiscard]] std::optional<Nullness> stateOf(PlaceId place) const;
  [[nodiscard]] bool mayBeNull(PlaceId place) const {
    const auto state = stateOf(place);
    return state && *state != Nullness::NonNull;
  }
  [[nodiscard]] bool isNonNull(PlaceId place) const {
    return stateOf(place) == Nullness::NonNull;
  }

  /// Forgets what is known about `place` (reassigned, overwritten, dead).
  void forget(PlaceId place);

  /// Per place (RFC 0008, *Nullness*, the join table): `MaybeNull` absorbs
  /// everything; `Null` with anything else is `MaybeNull`; `NonNull` with
  /// no fact is no fact. The record kept for a `MaybeNull` result is the one
  /// that said null (this side first); its guard is what the null sides'
  /// guards agree on, and it is `otherwiseNonNull` when every side that was
  /// not null was `NonNull` (RFC 0009). Returns whether this tracker changed.
  bool join(const NullTracker &other);

  /// `place` now satisfies `fact` (a condition edge): every null record's
  /// guard learns it. A `Null` record whose guard is refuted is erased (the
  /// path knows nothing); a `MaybeNull` one becomes `NonNull` if it was
  /// `otherwiseNonNull`, else is erased. Returns the places whose record
  /// changed state or vanished.
  std::vector<PlaceId> learn(PlaceId place, const ValueFact &fact);
  /// `place` was overwritten: no guard may speak about it any more.
  void dropGuardsOn(PlaceId place);

  /// Places with a record, ascending (for dumps).
  [[nodiscard]] std::vector<PlaceId> places() const;
  [[nodiscard]] const std::map<PlaceId, NullRecord> &all() const noexcept {
    return records;
  }

  [[nodiscard]] bool empty() const noexcept { return records.empty(); }

  friend bool operator==(const NullTracker &, const NullTracker &) = default;

private:
  std::map<PlaceId, NullRecord> records;
};

/// Stable spellings used in dumps: `null`, `maybe-null`, `nonnull`.
[[nodiscard]] std::string_view toString(Nullness state) noexcept;
/// `assigned-null`, `callee-result`, `callee-store`, `tested`, `declared`.
[[nodiscard]] std::string_view toString(NullReason reason) noexcept;

} // namespace weavec::core

#endif // WEAVEC_CORE_NULLNESS_H
