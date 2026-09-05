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

  /// The record of a copy at `step` from this pointer.
  [[nodiscard]] SpatialRecord derived(const PointerOffset &step) const {
    SpatialRecord result = *this;
    result.offset = offset.plus(step);
    return result;
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
    /// A constant access before the start of the object.
    BeforeStart,
  };
  Kind kind = Kind::OutOfBounds;
  /// For `MayBeOutOfBounds`: the value of `need.place - have.place` at the
  /// offending boundary (0 under `<=`, -1 under `<`). For
  /// `MayReachPastEnd`: the largest value of `need.place`.
  std::int64_t boundary = 0;

  friend bool operator==(const BoundsVerdict &,
                         const BoundsVerdict &) = default;
};

/// Compares an access needing `need` bytes past the start of an object of
/// `have` bytes (both affine in one integer place at most). `between` is
/// the relation `need.place REL have.place` known to hold, if any; the same
/// place on both sides is `Equal`. `needAtMost` and `haveAtMost` are the
/// constant upper bounds known on the two places (`RelationTracker::atMost`).
/// Nothing when the facts do not decide (an access proved in bounds and one
/// about which nothing is known are the same: no report).
[[nodiscard]] std::optional<BoundsVerdict>
boundsVerdict(const Affine &need, const Affine &have,
              std::optional<Relation> between,
              std::optional<std::int64_t> needAtMost = std::nullopt,
              std::optional<std::int64_t> haveAtMost = std::nullopt);

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
  /// is unknown from here on (the offset is kept).
  void dropExtentsOn(PlaceId counter);

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
