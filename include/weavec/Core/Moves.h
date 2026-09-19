//===- Moves.h - Move / deinitialization tracking --------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `MoveTracker` records which places have had their ownership moved out
// (including by being freed) so subsequent uses can be flagged.
//
// Every element of an array is one place (`a[*]`, RFC 0002). A move through
// an element access therefore carries an *element witness* (RFC 0006,
// *Element witnesses*): the constant or the index variable the access was
// spelled with. A later access is a use of the moved element only if its
// witness matches; `free(a[i])` followed by `a[j]` or, after `i++`, by
// `a[i]` may name a different element and is not reported.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_MOVES_H
#define WEAVEC_CORE_MOVES_H

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

/// Why a place became uninitialized.
enum class MoveReason : std::uint8_t {
  /// Ownership transferred elsewhere (assignment, passed by value, ...).
  Moved,
  /// The resource was released (e.g. `free`).
  Freed,
  /// The place was declared without an initialiser and nothing has been
  /// assigned to it yet (RFC 0008, *Uninitialised pointers*). Only locals
  /// carry this reason; it never reaches a summary.
  Uninitialized,
  /// RFC 0010: the place's share of a reference-counted object was released
  /// (`obj_unref(p)`); other shares, and the object, may live on. Reports as
  /// `use-after-free` / `double-free` with reference wording and reaches a
  /// summary as `freed,share`.
  Released,
};

/// Stable spelling used in dumps: `moved`, `freed`, `uninitialized`,
/// `released`.
[[nodiscard]] std::string_view toString(MoveReason reason) noexcept;

/// Which element of a summarised array place an access named.
struct ElementWitness {
  enum class Kind : std::uint8_t {
    /// The access named the place itself, without a subscript, or the fact
    /// comes from a summary: it applies to every element.
    Whole,
    /// A subscript that is an integer constant expression.
    Constant,
    /// A subscript that is a variable, unchanged since.
    Variable,
    /// An element that can no longer be identified: a computed subscript,
    /// a variable that was assigned, or a join of different witnesses.
    Unknown,
  };

  Kind kind = Kind::Whole;
  std::int64_t constant = 0;
  PlaceId variable;

  [[nodiscard]] static ElementWitness whole() noexcept { return {}; }
  [[nodiscard]] static ElementWitness unknown() noexcept {
    return ElementWitness{.kind = Kind::Unknown, .constant = 0, .variable = {}};
  }
  [[nodiscard]] static ElementWitness ofConstant(std::int64_t value) noexcept {
    return ElementWitness{
        .kind = Kind::Constant, .constant = value, .variable = {}};
  }
  [[nodiscard]] static ElementWitness ofVariable(PlaceId var) noexcept {
    return ElementWitness{
        .kind = Kind::Variable, .constant = 0, .variable = var};
  }

  [[nodiscard]] bool isWhole() const noexcept { return kind == Kind::Whole; }

  /// True if an access with witness `other` names the element this witness
  /// names: either is `Whole`, or both are the same constant or the same
  /// variable. `Unknown` matches nothing but `Whole`.
  [[nodiscard]] bool matches(const ElementWitness &other) const noexcept;

  friend bool operator==(const ElementWitness &,
                         const ElementWitness &) = default;
};

struct MoveRecord {
  MoveReason reason = MoveReason::Moved;
  SourceLocation location;
  /// The place named in the releasing/moving expression when it differs from
  /// the place this record is attached to, i.e. the move happened through an
  /// alias (`free(q)` marking `p`). Lets diagnostics say "freed here (through
  /// 'q')".
  std::optional<PlaceId> via;
  /// Which element the move named (RFC 0006); `Whole` for a plain place.
  ElementWitness element;
  /// The release family of the consume (RFC 0007), e.g. `free`; empty when
  /// unknown. Fed into the summary as the effect's family.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string family = {};
  /// The consumed value was one this function had itself stored in the
  /// place on every path since entry (the place was *overwritten*), so the
  /// record says nothing about the caller's value and never reaches a
  /// summary (RFC 0008, *Replaced values*). A join with a record that may be
  /// the caller's clears it.
  bool ownValue = false;
  /// RFC 0009: the move happened only when the guard holds: the facts that
  /// held on the path that consumed the place, and the callee's
  /// argument-conditional effect when a call did. Refuted by a later test,
  /// the record is gone.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PlaceGuard guard = {};
  /// RFC 0030 §3.1: every predecessor merged since the record was made had
  /// it.
  bool allPaths = true;
  /// RFC 0030 §3.1: made from a callee effect that holds only on some
  /// outcome classes or paths (a `PendingOutcome` class, or a summary effect
  /// that is not `consumesUnconditionally`). Cleared when a test of the
  /// result narrows the pending classes to ones that all consume the place,
  /// unless the effect is `lossy` (§9.1), in which case it is never cleared.
  bool conditional = false;
  /// RFC 0030 §9.1: made from a `lossy` effect; `conditional` stays set.
  bool lossy = false;
  /// RFC 0030 §3.1: made by the unknown-callee default (§5.1) or an open
  /// slot (§9.3). Never diagnosed.
  bool unknownOrigin = false;

  /// RFC 0030 §3.1: the record holds on every path that reaches a use, from
  /// a consume that happened on each of them. The `guard` of a record holds
  /// the facts of each path that made it, which held there by construction;
  /// a callee's condition on the arguments that the facts at the call left
  /// open makes the record `conditional` instead (so `guard.trivial()` of the
  /// RFC is the callee's part of the guard).
  [[nodiscard]] bool isDefinite() const noexcept {
    return allPaths && !conditional && !unknownOrigin;
  }

  friend bool operator==(const MoveRecord &, const MoveRecord &) = default;
};

/// RFC 0030 §3.1: how a consume came about, for the certainty of the record
/// it makes.
struct MoveOrigin {
  /// A callee effect that holds only on some outcome classes or paths.
  bool conditional = false;
  /// ... made by dropping a conjunct or folding classes (§9.1, stage S7).
  bool lossy = false;
  /// The unknown-callee default or an open slot (§5.1, §9.3).
  bool unknownOrigin = false;
};

/// Flow-insensitive record of moved-out places. Flow sensitivity is layered
/// on top by the analysis driver, which clones/joins trackers per CFG block.
class MoveTracker {
public:
  /// Marks `place` as moved out through an access with witness `element`.
  /// Returns the prior record if the place was already moved *and* the
  /// witnesses match (a double move / double free); the original record is
  /// kept. A prior record with a non-matching witness names another element
  /// and is replaced by the new one.
  /// `origin` gives the new record its RFC 0030 certainty bits.
  std::optional<MoveRecord>
  markMoved(PlaceId place, MoveReason reason, SourceLocation location,
            std::optional<PlaceId> via = {},
            ElementWitness element = ElementWitness::whole(),
            std::string family = {}, bool ownValue = false,
            PlaceGuard guard = {}, MoveOrigin origin = {});

  /// RFC 0030 §3.1: a test of the call's result selected only classes that
  /// consume `place`: its record is no longer conditional, unless it came
  /// from a lossy effect.
  void settleConditional(PlaceId place);
  /// RFC 0030 §3.1: `place`, already moved, was consumed again by an
  /// unconditional known consume (a second `free`) on a path with the facts
  /// `guard`: it is moved on every path through here, whatever the paths
  /// into the first consume were.
  void reaffirm(PlaceId place, PlaceGuard guard);

  /// Reinitializes `place`, e.g. after assignment of a fresh value. With a
  /// witness, only a record whose witness matches is erased (an element
  /// write does not reinitialise the other elements).
  void reinitialize(PlaceId place,
                    ElementWitness element = ElementWitness::whole());

  /// Returns the move record if `place` is currently moved out and the
  /// record's witness matches `element`.
  [[nodiscard]] std::optional<MoveRecord>
  movedAt(PlaceId place,
          ElementWitness element = ElementWitness::whole()) const;

  /// The record for `place` whatever its witness (for dumps and copies).
  [[nodiscard]] std::optional<MoveRecord> recordOf(PlaceId place) const;

  [[nodiscard]] bool isMoved(PlaceId place) const {
    return movedAt(place).has_value();
  }

  /// The variable `variable` was assigned: every record whose witness is
  /// that variable now names an unknown element.
  void forgetWitness(PlaceId variable);

  /// Merges another tracker into this one, keeping the union of moved places.
  /// This is the conservative "may be moved" join used at CFG merge points.
  /// Where both sides moved the same place, this side's record is kept, so
  /// the result does not depend on evaluation order; if the witnesses differ
  /// the kept record's witness becomes `Unknown`. A record on both sides is
  /// guarded by what its two guards agree on; one on one side keeps its own
  /// (RFC 0009). RFC 0030 §3.1: a record on one side only loses `allPaths`;
  /// one on both keeps it when both had it and agree on `unknownOrigin`, is
  /// `conditional` (and `lossy`) when either is, and `unknownOrigin` when
  /// both are. Returns whether this tracker changed.
  bool join(const MoveTracker &other);

  /// `place` now satisfies `fact` (a condition edge): every record's guard
  /// learns it; the records whose guard is refuted are erased and returned
  /// (RFC 0009, *Refuting guards*).
  std::vector<PlaceId> learn(PlaceId place, const ValueFact &fact);

  /// `place` was overwritten: no guard may speak about it any more.
  void dropGuardsOn(PlaceId place);
  /// RFC 0027: invalidate several overwritten values in one record scan.
  template <typename Matches>
  void dropGuardsIf(Matches matches) {
    for (auto &[holder, record] : moved)
      record.guard.dropIf(matches);
  }

  /// The record for `place` is now guarded by `guard` (used after a pending
  /// outcome narrowed the classes a guarded consume was attached to).
  void setGuard(PlaceId place, PlaceGuard guard);

  /// Moved places in ascending order (for dumps).
  [[nodiscard]] std::vector<PlaceId> movedPlaces() const;

  [[nodiscard]] bool empty() const noexcept { return moved.empty(); }

  friend bool operator==(const MoveTracker &, const MoveTracker &) = default;

private:
  std::map<PlaceId, MoveRecord> moved;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_MOVES_H
