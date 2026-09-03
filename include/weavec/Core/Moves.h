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
#include "weavec/Core/SourceLocation.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace weavec::core {

/// Why a place became uninitialized.
enum class MoveReason : std::uint8_t {
  /// Ownership transferred elsewhere (assignment, passed by value, ...).
  Moved,
  /// The resource was released (e.g. `free`).
  Freed,
};

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

  friend bool operator==(const MoveRecord &, const MoveRecord &) = default;
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
  std::optional<MoveRecord>
  markMoved(PlaceId place, MoveReason reason, SourceLocation location,
            std::optional<PlaceId> via = {},
            ElementWitness element = ElementWitness::whole(),
            std::string family = {});

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
  /// the kept record's witness becomes `Unknown`. Returns whether this
  /// tracker changed.
  bool join(const MoveTracker &other);

  /// Moved places in ascending order (for dumps).
  [[nodiscard]] std::vector<PlaceId> movedPlaces() const;

  [[nodiscard]] bool empty() const noexcept { return moved.empty(); }

  friend bool operator==(const MoveTracker &, const MoveTracker &) = default;

private:
  std::map<PlaceId, MoveRecord> moved;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_MOVES_H
