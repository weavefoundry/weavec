//===- Relation.cpp - Order relations between integer places --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Relation.h"

#include <algorithm>

namespace weavec::core {

Relation flipped(Relation relation) noexcept {
  switch (relation) {
  case Relation::Less:
    return Relation::Greater;
  case Relation::LessEqual:
    return Relation::GreaterEqual;
  case Relation::Equal:
    return Relation::Equal;
  case Relation::GreaterEqual:
    return Relation::LessEqual;
  case Relation::Greater:
    return Relation::Less;
  }
  return relation;
}

/// The three outcomes of comparing two values, as a bit set: a relation is
/// the set of outcomes it allows.
static unsigned outcomes(Relation relation) noexcept {
  constexpr unsigned Lt = 1;
  constexpr unsigned Eq = 2;
  constexpr unsigned Gt = 4;
  switch (relation) {
  case Relation::Less:
    return Lt;
  case Relation::LessEqual:
    return Lt | Eq;
  case Relation::Equal:
    return Eq;
  case Relation::GreaterEqual:
    return Eq | Gt;
  case Relation::Greater:
    return Gt;
  }
  return 0;
}

static std::optional<Relation> fromOutcomes(unsigned set) noexcept {
  switch (set) {
  case 1:
    return Relation::Less;
  case 3:
    return Relation::LessEqual;
  case 2:
    return Relation::Equal;
  case 6:
    return Relation::GreaterEqual;
  case 4:
    return Relation::Greater;
  default:
    // Empty (contradiction), `Lt | Gt` (not-equal) and everything (no
    // fact) are not relations this tracker keeps.
    return std::nullopt;
  }
}

std::optional<Relation> narrow(Relation a, Relation b) noexcept {
  return fromOutcomes(outcomes(a) & outcomes(b));
}

std::optional<Relation> widen(Relation a, Relation b) noexcept {
  return fromOutcomes(outcomes(a) | outcomes(b));
}

std::string_view spelling(Relation relation) noexcept {
  switch (relation) {
  case Relation::Less:
    return "<";
  case Relation::LessEqual:
    return "<=";
  case Relation::Equal:
    return "==";
  case Relation::GreaterEqual:
    return ">=";
  case Relation::Greater:
    return ">";
  }
  return "?";
}

// -- Edges as bounds on a difference ------------------------------------------
//
// RFC 0012, *Offset relations*: an edge `min REL max + k` bounds the
// difference `min - max` on one side or both (`Less` at `k` says `<= k -
// 1`; `Equal` says `== k`). Narrowing is the intersection of the bounds,
// joining their hull; either is spelled back as one edge when it is
// one-sided or a point, and is nothing this tracker keeps otherwise.

namespace {

struct Difference {
  std::optional<std::int64_t> lo;
  std::optional<std::int64_t> hi;
};

} // namespace

static std::optional<Difference> differenceOf(const RelationEdge &edge) {
  const std::int64_t k = edge.offset;
  switch (edge.relation) {
  case Relation::Less:
    if (k == INT64_MIN)
      return std::nullopt;
    return Difference{.lo = std::nullopt, .hi = k - 1};
  case Relation::LessEqual:
    return Difference{.lo = std::nullopt, .hi = k};
  case Relation::Equal:
    return Difference{.lo = k, .hi = k};
  case Relation::GreaterEqual:
    return Difference{.lo = k, .hi = std::nullopt};
  case Relation::Greater:
    if (k == INT64_MAX)
      return std::nullopt;
    return Difference{.lo = k + 1, .hi = std::nullopt};
  }
  return std::nullopt;
}

/// The one edge spelling `difference`, normalised so that the offset is
/// zero whenever a relation without one says the same thing (`<= -1` is
/// `Less` at 0, not `LessEqual` at -1: `between` reads those).
static std::optional<RelationEdge> edgeOf(const Difference &difference) {
  if (difference.lo && difference.hi) {
    if (*difference.lo > *difference.hi)
      return std::nullopt; // contradiction
    if (*difference.lo == *difference.hi)
      return RelationEdge{.relation = Relation::Equal,
                          .offset = *difference.lo};
    return std::nullopt; // two-sided: not one edge
  }
  if (difference.hi) {
    if (*difference.hi == -1)
      return RelationEdge{.relation = Relation::Less, .offset = 0};
    return RelationEdge{.relation = Relation::LessEqual,
                        .offset = *difference.hi};
  }
  if (difference.lo) {
    if (*difference.lo == 1)
      return RelationEdge{.relation = Relation::Greater, .offset = 0};
    return RelationEdge{.relation = Relation::GreaterEqual,
                        .offset = *difference.lo};
  }
  return std::nullopt;
}

static std::optional<RelationEdge> normalised(const RelationEdge &edge) {
  const auto difference = differenceOf(edge);
  if (!difference)
    return std::nullopt;
  return edgeOf(*difference);
}

void RelationTracker::learn(PlaceId lhs, Relation relation, PlaceId rhs,
                            std::int64_t offset) {
  if (lhs == rhs)
    return;
  RelationEdge edge{.relation = relation, .offset = offset};
  if (rhs < lhs) {
    std::swap(lhs, rhs);
    edge = edge.flipped();
  }
  const auto canonical = normalised(edge);
  if (!canonical)
    return;
  const auto key = std::make_pair(lhs, rhs);
  const auto it = pairs.find(key);
  if (it == pairs.end()) {
    pairs.emplace(key, *canonical);
    return;
  }
  const auto mine = differenceOf(it->second);
  const auto theirs = differenceOf(*canonical);
  if (mine && theirs) {
    Difference both;
    if (mine->lo && theirs->lo)
      both.lo = std::max(*mine->lo, *theirs->lo);
    else
      both.lo = mine->lo ? mine->lo : theirs->lo;
    if (mine->hi && theirs->hi)
      both.hi = std::min(*mine->hi, *theirs->hi);
    else
      both.hi = mine->hi ? mine->hi : theirs->hi;
    if (const auto narrowed = edgeOf(both)) {
      it->second = *narrowed;
      return;
    }
  }
  // A contradiction, or bounds on both sides one edge cannot spell: the
  // second fact wins.
  it->second = *canonical;
}

std::optional<RelationEdge> RelationTracker::directly(PlaceId lhs,
                                                      PlaceId rhs) const {
  const bool swapped = rhs < lhs;
  if (swapped)
    std::swap(lhs, rhs);
  const auto it = pairs.find(std::make_pair(lhs, rhs));
  if (it == pairs.end())
    return std::nullopt;
  return swapped ? it->second.flipped() : it->second;
}

std::vector<std::pair<PlaceId, std::int64_t>>
RelationTracker::equalsOf(PlaceId place) const {
  std::vector<std::pair<PlaceId, std::int64_t>> result;
  for (const auto &[pair, edge] : pairs) {
    if (edge.relation != Relation::Equal)
      continue;
    // `first == second + k`.
    if (pair.first == place)
      result.emplace_back(pair.second, edge.offset);
    else if (pair.second == place && edge.offset != INT64_MIN)
      result.emplace_back(pair.first, -edge.offset);
  }
  return result;
}

std::optional<RelationEdge> RelationTracker::edgeBetween(PlaceId lhs,
                                                         PlaceId rhs) const {
  if (lhs == rhs)
    return RelationEdge{.relation = Relation::Equal, .offset = 0};
  if (const auto direct = directly(lhs, rhs))
    return direct;
  const auto compose = [](const RelationEdge &edge,
                          std::int64_t shift) -> std::optional<RelationEdge> {
    std::int64_t offset = 0;
    if (__builtin_add_overflow(edge.offset, shift, &offset))
      return std::nullopt;
    return normalised(
        RelationEdge{.relation = edge.relation, .offset = offset});
  };
  // One hop through an equal place: `j = i + 1; if (j < n)` says `i < n -
  // 1` (`lhs == other + k1`, `other REL rhs + k2`: `lhs REL rhs + k1 + k2`).
  for (const auto &[other, k1] : equalsOf(lhs)) {
    if (other == rhs)
      continue;
    if (const auto via = directly(other, rhs))
      return compose(*via, k1);
  }
  // `rhs == other + k1`, `lhs REL other + k2`: `lhs REL rhs + k2 - k1`.
  for (const auto &[other, k1] : equalsOf(rhs)) {
    if (other == lhs || k1 == INT64_MIN)
      continue;
    if (const auto via = directly(lhs, other))
      return compose(*via, -k1);
  }
  return std::nullopt;
}

std::optional<Relation> RelationTracker::between(PlaceId lhs,
                                                 PlaceId rhs) const {
  const auto edge = edgeBetween(lhs, rhs);
  if (!edge || edge->offset != 0)
    return std::nullopt;
  return edge->relation;
}

void RelationTracker::noteBounded(PlaceId place) {
  bounded.insert(place);
}

bool RelationTracker::isBounded(PlaceId place) const {
  return bounded.contains(place);
}

void RelationTracker::learnAtMost(PlaceId place, std::int64_t bound) {
  bounded.insert(place);
  const auto it = upper.find(place);
  if (it == upper.end())
    upper.emplace(place, bound);
  else
    it->second = std::min(it->second, bound);
}

void RelationTracker::learnAtLeast(PlaceId place, std::int64_t bound) {
  bounded.insert(place);
  const auto it = lower.find(place);
  if (it == lower.end())
    lower.emplace(place, bound);
  else
    it->second = std::max(it->second, bound);
}

/// The bound of `place` in `bounds`, or of a place known equal to it with
/// the equality's offset applied (`j = i + 1; if (j < 8)` bounds `i` by 6).
static std::optional<std::int64_t> boundThroughEquals(
    PlaceId place, const std::map<PlaceId, std::int64_t> &bounds,
    const std::vector<std::pair<PlaceId, std::int64_t>> &equals) {
  if (const auto it = bounds.find(place); it != bounds.end())
    return it->second;
  for (const auto &[other, k] : equals) {
    const auto it = bounds.find(other);
    if (it == bounds.end())
      continue;
    std::int64_t shifted = 0;
    if (__builtin_add_overflow(it->second, k, &shifted))
      continue;
    return shifted;
  }
  return std::nullopt;
}

std::optional<std::int64_t> RelationTracker::atMost(PlaceId place) const {
  if (const auto it = upper.find(place); it != upper.end())
    return it->second;
  return boundThroughEquals(place, upper, equalsOf(place));
}

std::optional<std::int64_t> RelationTracker::atLeast(PlaceId place) const {
  if (const auto it = lower.find(place); it != lower.end())
    return it->second;
  return boundThroughEquals(place, lower, equalsOf(place));
}

bool RelationTracker::conditions(PlaceId place) const {
  if (bounded.contains(place))
    return true;
  return std::ranges::any_of(pairs, [place](const auto &entry) {
    return entry.first.first == place || entry.first.second == place;
  });
}

void RelationTracker::forget(PlaceId place) {
  bounded.erase(place);
  upper.erase(place);
  lower.erase(place);
  for (auto it = pairs.begin(); it != pairs.end();) {
    if (it->first.first == place || it->first.second == place)
      it = pairs.erase(it);
    else
      ++it;
  }
}

bool RelationTracker::join(const RelationTracker &other) {
  bool changed = false;
  for (auto it = pairs.begin(); it != pairs.end();) {
    const auto theirs = other.pairs.find(it->first);
    std::optional<RelationEdge> joined;
    if (theirs != other.pairs.end()) {
      const auto mine = differenceOf(it->second);
      const auto yours = differenceOf(theirs->second);
      if (mine && yours) {
        // The hull: a bound both sides have, as the looser one.
        Difference hull;
        if (mine->lo && yours->lo)
          hull.lo = std::min(*mine->lo, *yours->lo);
        if (mine->hi && yours->hi)
          hull.hi = std::max(*mine->hi, *yours->hi);
        joined = edgeOf(hull);
      }
    }
    if (!joined) {
      it = pairs.erase(it);
      changed = true;
      continue;
    }
    if (*joined != it->second) {
      it->second = *joined;
      changed = true;
    }
    ++it;
  }
  for (const PlaceId place : other.bounded)
    changed |= bounded.insert(place).second;
  for (auto it = upper.begin(); it != upper.end();) {
    const auto theirs = other.upper.find(it->first);
    if (theirs == other.upper.end()) {
      it = upper.erase(it);
      changed = true;
      continue;
    }
    if (theirs->second > it->second) {
      it->second = theirs->second;
      changed = true;
    }
    ++it;
  }
  for (auto it = lower.begin(); it != lower.end();) {
    const auto theirs = other.lower.find(it->first);
    if (theirs == other.lower.end()) {
      it = lower.erase(it);
      changed = true;
      continue;
    }
    if (theirs->second < it->second) {
      it->second = theirs->second;
      changed = true;
    }
    ++it;
  }
  return changed;
}

} // namespace weavec::core
