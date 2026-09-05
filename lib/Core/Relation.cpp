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

void RelationTracker::learn(PlaceId lhs, Relation relation, PlaceId rhs) {
  if (lhs == rhs)
    return;
  if (rhs < lhs) {
    std::swap(lhs, rhs);
    relation = flipped(relation);
  }
  const auto key = std::make_pair(lhs, rhs);
  const auto it = pairs.find(key);
  if (it == pairs.end()) {
    pairs.emplace(key, relation);
    return;
  }
  if (const auto narrowed = narrow(it->second, relation))
    it->second = *narrowed;
  else
    it->second = relation;
}

std::optional<Relation> RelationTracker::directly(PlaceId lhs,
                                                  PlaceId rhs) const {
  const bool swapped = rhs < lhs;
  if (swapped)
    std::swap(lhs, rhs);
  const auto it = pairs.find(std::make_pair(lhs, rhs));
  if (it == pairs.end())
    return std::nullopt;
  return swapped ? flipped(it->second) : it->second;
}

std::optional<Relation> RelationTracker::between(PlaceId lhs,
                                                 PlaceId rhs) const {
  if (lhs == rhs)
    return Relation::Equal;
  if (const auto direct = directly(lhs, rhs))
    return direct;
  // One hop through an equal place: `j = i; if (j < n)` says `i < n`.
  for (const auto &[pair, relation] : pairs) {
    if (relation != Relation::Equal)
      continue;
    std::optional<PlaceId> other;
    if (pair.first == lhs)
      other = pair.second;
    else if (pair.second == lhs)
      other = pair.first;
    if (!other || *other == rhs)
      continue;
    if (const auto viaOther = directly(*other, rhs))
      return viaOther;
  }
  for (const auto &[pair, relation] : pairs) {
    if (relation != Relation::Equal)
      continue;
    std::optional<PlaceId> other;
    if (pair.first == rhs)
      other = pair.second;
    else if (pair.second == rhs)
      other = pair.first;
    if (!other || *other == lhs)
      continue;
    if (const auto viaOther = directly(lhs, *other))
      return viaOther;
  }
  return std::nullopt;
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

std::optional<std::int64_t> RelationTracker::atMost(PlaceId place) const {
  if (const auto it = upper.find(place); it != upper.end())
    return it->second;
  // One hop through an equal place: `j = i; if (j < 8)` bounds `i`.
  for (const auto &[pair, relation] : pairs) {
    if (relation != Relation::Equal)
      continue;
    std::optional<PlaceId> other;
    if (pair.first == place)
      other = pair.second;
    else if (pair.second == place)
      other = pair.first;
    if (!other)
      continue;
    if (const auto it = upper.find(*other); it != upper.end())
      return it->second;
  }
  return std::nullopt;
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
    std::optional<Relation> joined;
    if (theirs != other.pairs.end())
      joined = widen(it->second, theirs->second);
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
  return changed;
}

} // namespace weavec::core
