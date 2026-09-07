//===- Spatial.h - Extents of objects and where pointers point -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0011, *Spatial records*. For a pointer place the checker may know the
// *extent* of the object it points into, in bytes, and the *offset* at
// which it points. The extent is affine in one integer place (`malloc(n *
// sizeof(T))` is `n * sizeof(T) + 0`) or a constant (`char buf[16]`); an
// access `p[i]` is checked against it where the checker can compare the two
// (RFC 0011, *Bounds checks*).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_SPATIAL_H
#define WEAVEC_CORE_SPATIAL_H

#include "weavec/Core/Offset.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Relation.h"
#include "weavec/Core/SourceLocation.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace weavec::core {

/// `scale * place + constant`, or `constant` alone when `place` is unset.
/// Units are bytes for an extent and for an access's need.
struct Affine {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PlaceId> place = {};
  std::int64_t scale = 1;
  std::int64_t constant = 0;

  [[nodiscard]] static Affine ofConstant(std::int64_t constant) noexcept {
    return Affine{.place = std::nullopt, .scale = 1, .constant = constant};
  }
  [[nodiscard]] static Affine ofPlace(PlaceId place, std::int64_t scale = 1,
                                      std::int64_t constant = 0) noexcept {
    return Affine{.place = place, .scale = scale, .constant = constant};
  }

  [[nodiscard]] bool isConstant() const noexcept { return !place; }
  /// This value scaled by `factor` (overflow makes it nothing).
  [[nodiscard]] std::optional<Affine> times(std::int64_t factor) const;
  /// This value plus `addend` (overflow makes it nothing).
  [[nodiscard]] std::optional<Affine> shifted(std::int64_t addend) const;

  /// `n*4+8`, `16`.
  [[nodiscard]] std::string toString() const;

  friend bool operator==(const Affine &, const Affine &) = default;
  friend std::strong_ordering operator<=>(const Affine &,
                                          const Affine &) = default;
};

/// RFC 0012, *String facts*: what the checker knows about the NUL-terminated
/// string the object holds. `length` set means a terminator lies that many
/// bytes from the object's start (so the object is terminated);
/// `unterminated` means no byte of the object is a NUL; neither means
/// unknown. Only `unterminated` is ever reported on; a length feeds the
/// needs of `strcpy`, `strcat` and `sprintf`.
struct StringFact {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<Affine> length = {};
  bool unterminated = false;
  /// Where the fact was established (the `strncpy`, the literal), for the
  /// note on a report.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SourceLocation location = {};

  [[nodiscard]] bool empty() const noexcept { return !length && !unterminated; }

  friend bool operator==(const StringFact &, const StringFact &) = default;
};

/// What the checker knows about the object a pointer place points into.
struct SpatialRecord {
  /// Bytes of the object from its start; nothing when unknown.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<Affine> extent = {};
  /// Where in the object the pointer points.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  PointerOffset offset = {};
  /// Where the extent was established (the allocation, the declaration),
  /// for the note on a report.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SourceLocation location = {};
  /// Whether `location` is a declaration (a variable's storage, an
  /// annotated parameter) rather than an allocation.
  bool declared = false;
  /// RFC 0012: the string the object holds, when anything is known.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<StringFact> string = {};

  /// RFC 0017: bounds may be relative to a subobject while `offset` keeps
  /// the enclosing allocation's lifetime/release identity.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<PointerOffset> boundsOffset = {};

  /// The record of a copy at `step` from this pointer.
  [[nodiscard]] SpatialRecord derived(const PointerOffset &step) const {
    SpatialRecord result = *this;
    result.offset = offset.plus(step);
    if (boundsOffset)
      result.boundsOffset = boundsOffset->plus(step);
    return result;
  }

  /// True if nothing is known: no extent, a zero offset, no string fact.
  [[nodiscard]] bool empty() const noexcept {
    return !extent && offset.isZero() && !string;
  }

  friend bool operator==(const SpatialRecord &,
                         const SpatialRecord &) = default;
};

/// RFC 0011, *Bounds checks*: the outcome of comparing what an access needs
/// with what the object has.
struct BoundsVerdict {
  enum class Kind : std::uint8_t {
    /// Every value the facts allow puts the access past the end.
    OutOfBounds,
    /// The boundary value the facts allow puts it past the end (`p[i]`
    /// under `i <= n`; `p[i + 1]` under `i < n`).
    MayBeOutOfBounds,
    /// The largest value the index may take (`i < 8` says `7`) puts it
    /// past the end of an object of constant size.
    MayReachPastEnd,
    /// A constant access before the start of the object, or one whose
    /// index is bounded above so that it ends at or before the start
    /// (RFC 0012: `i <= -1` then `p[i]`).
    BeforeStart,
    /// RFC 0012: the smallest value the index may take (`i >= 8`) is
    /// already past the end of an object of constant size: every value is.
    AtLeastPastEnd,
  };
  Kind kind = Kind::OutOfBounds;
  /// For `MayBeOutOfBounds`: the value of `need.place - have.place` at the
  /// offending boundary (0 under `<=`, -1 under `<`). For
  /// `MayReachPastEnd`: the largest value of `need.place`. For
  /// `AtLeastPastEnd`: the smallest.
  std::int64_t boundary = 0;

  friend bool operator==(const BoundsVerdict &,
                         const BoundsVerdict &) = default;
};

/// The constant bounds known on the places of a `need` and a `have`
/// (`RelationTracker::atMost` / `atLeast`), for `boundsVerdict`.
struct KnownBounds {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<std::int64_t> needAtMost = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<std::int64_t> haveAtMost = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<std::int64_t> needAtLeast = {};
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<std::int64_t> haveAtLeast = {};
  /// RFC 0017: an abstract type/range endpoint is not a reachable witness.
  bool needBoundaryWitness = true;
};

/// Compares an access needing `need` bytes past the start of an object of
/// `have` bytes (both affine in one integer place at most). `between` is
/// the relation `need.place REL have.place` known to hold, if any; the same
/// place on both sides is `Equal`. `bounds` carries the constant bounds
/// known on the two places. Nothing when the facts do not decide (an
/// access proved in bounds and one about which nothing is known are the
/// same: no report). Use checkSpatialBounds when proof coverage matters.
[[nodiscard]] std::optional<BoundsVerdict>
boundsVerdict(const Affine &need, const Affine &have,
              std::optional<Relation> between, const KnownBounds &bounds = {});

/// RFC 0017: absence of a violation does not establish bounds safety.
enum class SpatialOutcome : std::uint8_t { Proven, Violation, Unresolved };
enum class SpatialReason : std::uint8_t {
  None,
  UnknownExtent,
  UnknownOffset,
  UnknownIndex,
  Arithmetic,
  UnsupportedExpression,
  InterfaceRequirement
};
[[nodiscard]] std::string_view toString(SpatialOutcome outcome) noexcept;
[[nodiscard]] std::string_view toString(SpatialReason reason) noexcept;
struct SpatialCheck {
  SpatialOutcome outcome = SpatialOutcome::Unresolved;
  SpatialReason reason = SpatialReason::UnknownIndex;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::optional<BoundsVerdict> violation = {};
  friend bool operator==(const SpatialCheck &, const SpatialCheck &) = default;
};
/// `start` is the first accessed byte, `need` the exclusive end. Quantities
/// here are mathematical bytes, never target-width modular operations.
[[nodiscard]] SpatialCheck
checkSpatialBounds(const Affine &start, const Affine &need, const Affine &have,
                   std::optional<Relation> between,
                   const KnownBounds &bounds = {},
                   std::optional<std::int64_t> startAtLeast = {});

/// Flow-sensitive map from pointer places to their spatial records; cloned
/// and joined per CFG block like every other component of the state.
class SpatialTracker {
public:
  void set(PlaceId place, SpatialRecord record);
  [[nodiscard]] std::optional<SpatialRecord> recordOf(PlaceId place) const;
  [[nodiscard]] bool has(PlaceId place) const {
    return records.contains(place);
  }
  void forget(PlaceId place);

  /// The integer place `counter` was written: every extent expressed in it
  /// is unknown from here on (the offset is kept), and so is every string
  /// length (RFC 0012).
  void dropExtentsOn(PlaceId counter);

  /// RFC 0012: sets the string fact of `place`'s object (a record with no
  /// extent is created when there is none).
  void setString(PlaceId place, std::optional<StringFact> fact);
  /// RFC 0012: the object behind `place` was written in a way the string
  /// tracker does not follow: its string fact is unknown.
  void dropStringFacts(PlaceId place);

  /// Per place: a record on both sides keeps its extent only when they
  /// agree and joins the offsets; a record on one side only is dropped (a
  /// bounds fact must hold on every path in). Returns whether this changed.
  bool join(const SpatialTracker &other);

  [[nodiscard]] const std::map<PlaceId, SpatialRecord> &all() const noexcept {
    return records;
  }

  friend bool operator==(const SpatialTracker &,
                         const SpatialTracker &) = default;

private:
  std::map<PlaceId, SpatialRecord> records;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_SPATIAL_H
