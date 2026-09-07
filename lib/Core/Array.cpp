//===- Array.cpp - Bounded array selections and intervals ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Array.h"

#include <charconv>
#include <limits>

namespace weavec::core {

std::optional<ArrayIndex> ArrayIndex::shifted(std::int64_t by) const {
  auto result = *this;
  if (__builtin_add_overflow(offset, by, &result.offset))
    return std::nullopt;
  return result;
}

std::optional<std::int64_t>
ArrayIndex::difference(const ArrayIndex &other) const {
  std::int64_t result = 0;
  if (symbol != other.symbol ||
      __builtin_sub_overflow(offset, other.offset, &result))
    return std::nullopt;
  return result;
}

std::string ArrayIndex::toString() const {
  if (!symbol)
    return std::to_string(offset);
  std::string text = "$" + std::to_string(*symbol);
  if (offset != 0)
    text += (offset > 0 ? "+" : "") + std::to_string(offset);
  return text;
}

std::optional<ArrayIndex> ArrayIndex::parse(std::string_view text) {
  if (text.empty())
    return std::nullopt;
  ArrayIndex result;
  if (text.front() == '$') {
    text.remove_prefix(1);
    std::uint32_t id = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr == text.data())
      return std::nullopt;
    result.symbol = id;
    text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
    if (text.empty())
      return result;
    if (text.front() == '+') {
      text.remove_prefix(1);
      if (text.empty() || text.front() == '-')
        return std::nullopt;
    } else if (text.front() != '-') {
      return std::nullopt;
    }
  }
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result.offset);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    return std::nullopt;
  return result;
}

std::optional<std::int64_t> ArrayInterval::length() const {
  const auto result = end.difference(begin);
  return result && *result >= 0 ? result : std::nullopt;
}

ArrayRelation ArrayInterval::contains(const ArrayIndex &index) const {
  const auto lo = index.difference(begin);
  const auto hi = index.difference(end);
  if ((lo && *lo < 0) || (hi && *hi >= 0))
    return ArrayRelation::No;
  if (lo && hi)
    return ArrayRelation::Yes;
  if (const auto count = length(); count && *count == 0)
    return ArrayRelation::No;
  return ArrayRelation::Unknown;
}

ArrayRelation ArrayInterval::overlaps(const ArrayInterval &other) const {
  if ((length() && *length() == 0) || (other.length() && *other.length() == 0))
    return ArrayRelation::No;
  const auto before = end.difference(other.begin);
  const auto after = other.end.difference(begin);
  if ((before && *before <= 0) || (after && *after <= 0))
    return ArrayRelation::No;
  if (before && after)
    return ArrayRelation::Yes;
  return ArrayRelation::Unknown;
}

std::optional<ArrayInterval> ArrayInterval::shifted(std::int64_t by) const {
  const auto first = begin.shifted(by);
  const auto last = end.shifted(by);
  if (!first || !last)
    return std::nullopt;
  return ArrayInterval{.begin = *first, .end = *last};
}

std::optional<ArrayIndex> translateArrayIndex(const ArrayIndex &index,
                                              const ArrayIndex &from,
                                              const ArrayIndex &to) {
  if (const auto offset = index.difference(from))
    return to.shifted(*offset);
  if (const auto offset = to.difference(from))
    return index.shifted(*offset);
  return std::nullopt;
}

static Affine foldArrayAffine(Affine value, const ScalarTracker &scalars) {
  if (value.place)
    if (const auto fact = scalars.factOf(*value.place);
        fact && fact->constant) {
      std::int64_t scaled = 0;
      std::int64_t constant = 0;
      if (!__builtin_mul_overflow(*fact->constant, value.scale, &scaled) &&
          !__builtin_add_overflow(scaled, value.constant, &constant))
        return Affine::ofConstant(constant);
    }
  return value;
}

/// Prove a <= b using exact values, existing order edges and sign/bound
/// facts. Absence of a counterexample is not a successful proof.
static bool arrayAtMost(Affine a, Affine b, const ScalarTracker &scalars,
                        const RelationTracker &relations) {
  a = foldArrayAffine(a, scalars);
  b = foldArrayAffine(b, scalars);
  if (a.place == b.place && (!a.place || a.scale == b.scale))
    return a.constant <= b.constant;
  if (a.place && b.place && a.scale == 1 && b.scale == 1) {
    const auto edge = relations.edgeBetween(*a.place, *b.place);
    if (!edge || (edge->relation != Relation::Less &&
                  edge->relation != Relation::LessEqual &&
                  edge->relation != Relation::Equal))
      return false;
    auto limit = Affine::ofConstant(edge->offset).shifted(a.constant);
    if (limit && edge->relation == Relation::Less)
      limit = limit->shifted(-1);
    return limit && limit->constant <= b.constant;
  }
  const auto bound = [&](const Affine &value,
                         bool upper) -> std::optional<std::int64_t> {
    if (!value.place)
      return value.constant;
    if (value.scale != 1)
      return std::nullopt;
    auto result = upper ? relations.atMost(*value.place)
                        : relations.atLeast(*value.place);
    if (!result)
      if (const auto fact = scalars.factOf(*value.place)) {
        if (upper && !fact->classes.contains(Outcome::Positive))
          result = fact->classes.contains(Outcome::Zero) ? 0 : -1;
        if (!upper && !fact->classes.contains(Outcome::Negative))
          result = fact->classes.contains(Outcome::Zero) ? 0 : 1;
      }
    if (!result)
      return std::nullopt;
    const auto shifted = Affine::ofConstant(*result).shifted(value.constant);
    return shifted ? std::optional(shifted->constant) : std::nullopt;
  };
  const auto upper = bound(a, true);
  const auto lower = bound(b, false);
  return upper && lower && *upper <= *lower;
}

ArrayRelation ArraySpan::contains(const ArrayIndex &index,
                                  const ScalarTracker &scalars,
                                  const RelationTracker &relations) const {
  const auto relative =
      translateArrayIndex(index, begin, ArrayIndex::constant(0));
  if (!relative)
    return ArrayRelation::Unknown;
  const auto offset =
      relative->symbol
          ? Affine::ofPlace(PlaceId{*relative->symbol}, 1, relative->offset)
          : Affine::ofConstant(relative->offset);
  const auto next = offset.shifted(1);
  if (arrayAtMost(offset, Affine::ofConstant(-1), scalars, relations) ||
      arrayAtMost(count, offset, scalars, relations))
    return ArrayRelation::No;
  if (next && arrayAtMost(Affine::ofConstant(0), offset, scalars, relations) &&
      arrayAtMost(*next, count, scalars, relations))
    return ArrayRelation::Yes;
  return ArrayRelation::Unknown;
}

bool arrayIndicesDisjoint(const ArrayIndex &a, const ArrayIndex &b,
                          const ScalarTracker &scalars,
                          const RelationTracker &relations) {
  if (const auto difference = a.difference(b))
    return *difference != 0;
  if (ArraySpan{.begin = a, .count = Affine::ofConstant(1)}.contains(
          b, scalars, relations) == ArrayRelation::No ||
      ArraySpan{.begin = b, .count = Affine::ofConstant(1)}.contains(
          a, scalars, relations) == ArrayRelation::No)
    return true;
  if (!a.symbol || !b.symbol)
    return false;
  const auto edge =
      relations.edgeBetween(PlaceId{*a.symbol}, PlaceId{*b.symbol});
  if (!edge)
    return false;
  std::int64_t distance = 0;
  if (__builtin_add_overflow(edge->offset, a.offset, &distance) ||
      __builtin_sub_overflow(distance, b.offset, &distance))
    return false;
  switch (edge->relation) {
  case Relation::Equal:
    return distance != 0;
  case Relation::Less:
    return distance <= 0;
  case Relation::LessEqual:
    return distance < 0;
  case Relation::Greater:
    return distance >= 0;
  case Relation::GreaterEqual:
    return distance > 0;
  }
  return false;
}

} // namespace weavec::core
