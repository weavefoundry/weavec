//===- Spatial.cpp - Extents of objects and where pointers point ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Spatial.h"

#include <limits>

namespace weavec::core {

static bool mulOverflows(std::int64_t a, std::int64_t b) {
  if (a == 0 || b == 0)
    return false;
  const std::int64_t product = a * b;
  return product / b != a ||
         (a == -1 && b == std::numeric_limits<std::int64_t>::min()) ||
         (b == -1 && a == std::numeric_limits<std::int64_t>::min());
}

static bool addOverflows(std::int64_t a, std::int64_t b) {
  return (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
         (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b);
}

std::optional<Affine> Affine::times(std::int64_t factor) const {
  if (mulOverflows(constant, factor) || (place && mulOverflows(scale, factor)))
    return std::nullopt;
  Affine result = *this;
  result.constant *= factor;
  if (place)
    result.scale *= factor;
  return result;
}

std::optional<Affine> Affine::shifted(std::int64_t addend) const {
  if (addOverflows(constant, addend))
    return std::nullopt;
  Affine result = *this;
  result.constant += addend;
  return result;
}

std::string Affine::toString() const {
  if (!place)
    return std::to_string(constant);
  std::string text = "p" + std::to_string(place->value);
  if (scale != 1)
    text += "*" + std::to_string(scale);
  if (constant != 0)
    text += (constant > 0 ? "+" : "") + std::to_string(constant);
  return text;
}

/// `scale * value + constant`, unless it overflows.
static std::optional<std::int64_t> valueAt(const Affine &affine,
                                           std::int64_t value) {
  std::int64_t scaled = 0;
  if (__builtin_mul_overflow(affine.scale, value, &scaled))
    return std::nullopt;
  std::int64_t total = 0;
  if (__builtin_add_overflow(scaled, affine.constant, &total))
    return std::nullopt;
  return total;
}

std::optional<BoundsVerdict>
boundsVerdict(const Affine &need, const Affine &have,
              std::optional<Relation> between,
              std::optional<std::int64_t> needAtMost,
              std::optional<std::int64_t> haveAtMost) {
  // 5: a constant access that ends at or before the start began before it
  // (the need counts the bytes of the element itself).
  if (need.isConstant() && need.constant <= 0)
    return BoundsVerdict{.kind = BoundsVerdict::Kind::BeforeStart};
  // 1: two constants.
  if (need.isConstant() && have.isConstant()) {
    if (need.constant > have.constant)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::OutOfBounds};
    return std::nullopt;
  }
  // 6: a constant on one side against a place bounded above on the other.
  // An object of at most `U` bytes cannot hold a constant access past `U`;
  // an index that may reach `U` may reach past an object of constant size.
  if (need.isConstant() != have.isConstant()) {
    if (need.isConstant() && haveAtMost && have.scale > 0) {
      const auto largest = valueAt(have, *haveAtMost);
      if (largest && need.constant > *largest)
        return BoundsVerdict{.kind = BoundsVerdict::Kind::OutOfBounds};
      return std::nullopt;
    }
    if (have.isConstant() && needAtMost && need.scale > 0) {
      const auto largest = valueAt(need, *needAtMost);
      if (largest && *largest > have.constant)
        return BoundsVerdict{.kind = BoundsVerdict::Kind::MayReachPastEnd,
                             .boundary = *needAtMost};
      return std::nullopt;
    }
    return std::nullopt;
  }
  if (*need.place == *have.place)
    between = Relation::Equal;
  if (!between)
    return std::nullopt;
  // The scale is the element size on both sides when the pointer walks the
  // object it was allocated as; anything else is not compared.
  if (need.scale != have.scale || need.scale <= 0)
    return std::nullopt;
  switch (*between) {
  case Relation::Equal:
  case Relation::GreaterEqual:
    // 2, 3: `i >= n`: `scale*i + c >= scale*n + c > scale*n + hc`.
    if (need.constant > have.constant)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::OutOfBounds};
    return std::nullopt;
  case Relation::Greater:
    // 3: `i >= n + 1`.
    if (need.constant + need.scale > have.constant)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::OutOfBounds};
    return std::nullopt;
  case Relation::LessEqual:
    // 4: `i = n` is allowed and is past the end.
    if (need.constant > have.constant)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::MayBeOutOfBounds,
                           .boundary = 0};
    return std::nullopt;
  case Relation::Less:
    // 4: `i = n - 1` is allowed and is past the end (`p[i + 1]`).
    if (need.constant - need.scale > have.constant)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::MayBeOutOfBounds,
                           .boundary = -1};
    return std::nullopt;
  }
  return std::nullopt;
}

void SpatialTracker::set(PlaceId place, SpatialRecord record) {
  records.insert_or_assign(place, std::move(record));
}

std::optional<SpatialRecord> SpatialTracker::recordOf(PlaceId place) const {
  const auto it = records.find(place);
  if (it == records.end())
    return std::nullopt;
  return it->second;
}

void SpatialTracker::forget(PlaceId place) {
  records.erase(place);
}

void SpatialTracker::dropExtentsOn(PlaceId counter) {
  for (auto &[place, record] : records) {
    if (record.extent && record.extent->place == counter)
      record.extent.reset();
  }
}

bool SpatialTracker::join(const SpatialTracker &other) {
  // A place without a record stands at the start of an object of unknown
  // extent: joining with one that has a record keeps the offset's join (a
  // pointer that stepped on one path only "may not point to the start").
  static const SpatialRecord Absent{
      .extent = std::nullopt, .offset = {}, .location = {}};
  bool changed = false;
  for (auto it = records.begin(); it != records.end();) {
    const auto found = other.records.find(it->first);
    const SpatialRecord &theirs =
        found == other.records.end() ? Absent : found->second;
    SpatialRecord &mine = it->second;
    if (mine.extent != theirs.extent && mine.extent) {
      mine.extent.reset();
      changed = true;
    }
    changed |= mine.offset.join(theirs.offset);
    if (mine == Absent) {
      it = records.erase(it);
      continue;
    }
    ++it;
  }
  for (const auto &[place, theirs] : other.records) {
    if (records.contains(place))
      continue;
    SpatialRecord mine = Absent;
    mine.offset.join(theirs.offset);
    if (mine == Absent)
      continue;
    records.emplace(place, std::move(mine));
    changed = true;
  }
  return changed;
}

} // namespace weavec::core
