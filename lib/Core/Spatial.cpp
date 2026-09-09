//===- Spatial.cpp - Extents of objects and where pointers point ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Spatial.h"

namespace weavec::core {

// Signed overflow is undefined, so the check cannot be done on the wrapped
// result (an optimising build folds it away); the builtins report it.
static bool mulOverflows(std::int64_t a, std::int64_t b) {
  std::int64_t product = 0;
  return __builtin_mul_overflow(a, b, &product);
}

static bool addOverflows(std::int64_t a, std::int64_t b) {
  std::int64_t sum = 0;
  return __builtin_add_overflow(a, b, &sum);
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

std::optional<BoundsVerdict> boundsVerdict(const Affine &need,
                                           const Affine &have,
                                           std::optional<Relation> between,
                                           const KnownBounds &bounds) {
  // 5: a constant access that ends at or before the start began before it
  // (the need counts the bytes of the element itself).
  if (need.isConstant() && need.constant <= 0)
    return BoundsVerdict{.kind = BoundsVerdict::Kind::BeforeStart};
  // 5' (RFC 0012): an index bounded above so that the access ends at or
  // before the start on every value allowed (`i <= -1` then `p[i]`).
  if (!need.isConstant() && bounds.needAtMost && need.scale > 0) {
    const auto largest = valueAt(need, *bounds.needAtMost);
    if (largest && *largest <= 0)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::BeforeStart,
                           .boundary = *bounds.needAtMost};
  }
  // 1: two constants.
  if (need.isConstant() && have.isConstant()) {
    if (need.constant > have.constant)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::OutOfBounds};
    return std::nullopt;
  }
  // 6: a constant on one side against a place bounded above on the other.
  // An object of at most `U` bytes cannot hold a constant access past `U`;
  // an index that may reach `U` may reach past an object of constant size.
  // 3' (RFC 0012): an index bounded *below* by `L` whose smallest access is
  // already past an object of constant size is out of bounds outright.
  if (need.isConstant() != have.isConstant()) {
    if (need.isConstant() && bounds.haveAtMost && have.scale > 0) {
      const auto largest = valueAt(have, *bounds.haveAtMost);
      if (largest && need.constant > *largest)
        return BoundsVerdict{.kind = BoundsVerdict::Kind::OutOfBounds};
      return std::nullopt;
    }
    if (have.isConstant() && need.scale > 0) {
      if (bounds.needAtLeast) {
        const auto smallest = valueAt(need, *bounds.needAtLeast);
        if (smallest && *smallest > have.constant)
          return BoundsVerdict{.kind = BoundsVerdict::Kind::AtLeastPastEnd,
                               .boundary = *bounds.needAtLeast};
      }
      if (bounds.needAtMost && bounds.needBoundaryWitness) {
        const auto largest = valueAt(need, *bounds.needAtMost);
        if (largest && *largest > have.constant)
          return BoundsVerdict{.kind = BoundsVerdict::Kind::MayReachPastEnd,
                               .boundary = *bounds.needAtMost};
      }
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
    if (const auto next = need.shifted(need.scale);
        next && next->constant > have.constant)
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
    if (const auto previous = need.shifted(-need.scale);
        previous && previous->constant > have.constant)
      return BoundsVerdict{.kind = BoundsVerdict::Kind::MayBeOutOfBounds,
                           .boundary = -1};
    return std::nullopt;
  }
  return std::nullopt;
}

std::string_view toString(SpatialOutcome outcome) noexcept {
  switch (outcome) {
  case SpatialOutcome::Proven:
    return "proven";
  case SpatialOutcome::Violation:
    return "violation";
  case SpatialOutcome::Unresolved:
    return "unresolved";
  }
  return "unresolved";
}
std::string_view toString(SpatialReason reason) noexcept {
  switch (reason) {
  case SpatialReason::None:
    return "none";
  case SpatialReason::UnknownExtent:
    return "unknown extent";
  case SpatialReason::UnknownOffset:
    return "unknown pointer offset";
  case SpatialReason::UnknownIndex:
    return "unknown index bounds";
  case SpatialReason::Arithmetic:
    return "unrepresentable byte arithmetic";
  case SpatialReason::UnsupportedExpression:
    return "unsupported numeric expression";
  case SpatialReason::InterfaceRequirement:
    return "caller requirement";
  }
  return "unsupported numeric expression";
}
SpatialCheck checkSpatialBounds(const Affine &start, const Affine &need,
                                const Affine &have,
                                std::optional<Relation> between,
                                const KnownBounds &bounds,
                                std::optional<std::int64_t> startAtLeast) {
  std::optional<std::int64_t> lower;
  if (start.isConstant())
    lower = start.constant;
  else if (startAtLeast && start.scale >= 0)
    lower = valueAt(start, *startAtLeast);
  const auto violation = boundsVerdict(need, have, between, bounds);
  if (violation)
    return {.outcome = SpatialOutcome::Violation,
            .reason = SpatialReason::None,
            .violation = violation};
  // A negative first byte is invalid even if the access straddles zero.
  if (start.isConstant() && start.constant < 0)
    return {.outcome = SpatialOutcome::Violation,
            .reason = SpatialReason::None,
            .violation =
                BoundsVerdict{.kind = BoundsVerdict::Kind::BeforeStart}};
  if (!lower || *lower < 0)
    return {};
  std::optional<std::int64_t> largest;
  if (need.isConstant())
    largest = need.constant;
  else if (bounds.needAtMost && need.scale >= 0)
    largest = valueAt(need, *bounds.needAtMost);
  std::optional<std::int64_t> smallest;
  if (have.isConstant())
    smallest = have.constant;
  else if (bounds.haveAtLeast && have.scale >= 0)
    smallest = valueAt(have, *bounds.haveAtLeast);
  bool safe = largest && smallest && *largest <= *smallest;
  if (need.place && have.place && need.scale == have.scale && need.scale > 0) {
    if (need.place == have.place)
      between = Relation::Equal;
    if (between == Relation::Equal || between == Relation::LessEqual)
      safe |= need.constant <= have.constant;
    if (between == Relation::Less) {
      std::int64_t delta = 0;
      if (!__builtin_sub_overflow(need.constant, need.scale, &delta))
        safe |= delta <= have.constant;
    }
  }
  return safe ? SpatialCheck{.outcome = SpatialOutcome::Proven,
                             .reason = SpatialReason::None}
              : SpatialCheck{};
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
    if (record.string && record.string->length &&
        record.string->length->place == counter) {
      record.string.reset();
      if (record.empty())
        record.location = {};
    }
  }
}

void SpatialTracker::setString(PlaceId place, std::optional<StringFact> fact) {
  if (fact && fact->empty())
    fact.reset();
  const auto it = records.find(place);
  if (it == records.end()) {
    if (!fact)
      return;
    records.emplace(place, SpatialRecord{.string = std::move(fact)});
    return;
  }
  it->second.string = std::move(fact);
}

void SpatialTracker::dropStringFacts(PlaceId place) {
  const auto it = records.find(place);
  if (it == records.end())
    return;
  it->second.string.reset();
}

/// The string fact both paths agree on: the same fact, or `unterminated`
/// when both say so (the locations may differ; the first is kept), or a
/// length both know equal. Anything else is unknown.
static bool joinString(std::optional<StringFact> &mine,
                       const std::optional<StringFact> &theirs) {
  if (mine == theirs || !mine)
    return false;
  if (theirs) {
    if (mine->unterminated && theirs->unterminated)
      return false;
    if (mine->length && mine->length == theirs->length && !mine->unterminated &&
        !theirs->unterminated)
      return false;
  }
  mine.reset();
  return true;
}

static bool joinRecord(SpatialRecord &mine, const SpatialRecord &theirs) {
  bool changed = false;
  if (mine.extent != theirs.extent && mine.extent) {
    mine.extent.reset();
    changed = true;
  }
  if (mine.boundsOffset != theirs.boundsOffset) {
    if (mine.boundsOffset && theirs.boundsOffset) {
      changed |= mine.boundsOffset->join(*theirs.boundsOffset);
    } else if (mine.boundsOffset) {
      mine.boundsOffset.reset();
      changed = true;
    }
  }
  changed |= mine.offset.join(theirs.offset);
  changed |= joinString(mine.string, theirs.string);
  return changed;
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
    changed |= joinRecord(mine, theirs);
    if (mine.empty()) {
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

bool SpatialTracker::joinWithAbsentObjects(
    const SpatialTracker &other, const std::function<bool(PlaceId)> &absentHere,
    const std::function<bool(PlaceId)> &absentThere) {
  if (this == &other)
    return std::erase_if(records, [](const auto &entry) {
             return entry.second.empty();
           }) != 0;
  static const SpatialRecord Absent{};
  bool changed = false;
  auto mine = records.begin();
  auto theirs = other.records.begin();
  while (mine != records.end() || theirs != other.records.end()) {
    if (theirs == other.records.end() ||
        (mine != records.end() && mine->first < theirs->first)) {
      if (!absentThere(mine->first))
        changed |= joinRecord(mine->second, Absent);
      if (mine->second.empty()) {
        mine = records.erase(mine);
        changed = true;
      } else {
        ++mine;
      }
      continue;
    }
    if (mine == records.end() || theirs->first < mine->first) {
      SpatialRecord record;
      if (absentHere(theirs->first)) {
        record = theirs->second;
      } else {
        record.offset.join(theirs->second.offset);
      }
      if (!record.empty()) {
        records.emplace_hint(mine, theirs->first, std::move(record));
        changed = true;
      }
      ++theirs;
      continue;
    }
    changed |= joinRecord(mine->second, theirs->second);
    if (mine->second.empty()) {
      mine = records.erase(mine);
      changed = true;
    } else {
      ++mine;
    }
    ++theirs;
  }
  return changed;
}

} // namespace weavec::core
