//===- Traversal.cpp - Checked traversal relations (RFC 0021) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Traversal.h"

#include <algorithm>
#include <limits>
#include <set>

namespace weavec::core {

void DifferenceConstraints::learn(PlaceId lhs, RelationEdge edge, PlaceId rhs) {
  auto limit = edge.offset;
  if (edge.relation == Relation::Less &&
      __builtin_sub_overflow(limit, std::int64_t{1}, &limit))
    return;
  if (edge.relation == Relation::Less || edge.relation == Relation::LessEqual ||
      edge.relation == Relation::Equal)
    constrain(lhs, rhs, limit);
  limit = edge.offset;
  if (__builtin_sub_overflow(std::int64_t{0}, limit, &limit) ||
      (edge.relation == Relation::Greater &&
       __builtin_sub_overflow(limit, std::int64_t{1}, &limit)))
    return;
  if (edge.relation == Relation::Greater ||
      edge.relation == Relation::GreaterEqual ||
      edge.relation == Relation::Equal)
    constrain(rhs, lhs, limit);
}

bool DifferenceConstraints::constrain(DifferenceTerm x, DifferenceTerm y,
                                      std::int64_t bound) {
  const Key key{x, y};
  if (const auto found = constraints.find(key); found != constraints.end()) {
    if (found->second <= bound)
      return false;
    found->second = bound;
    return true;
  }
  if (constraints.size() >= MaxTraversalSteps) {
    exhausted = true;
    return false;
  }
  // An edge introduces at most two variables. After that cheap bound stops
  // being sufficient, recount only when an endpoint could actually be new.
  // The zero term never consumes a variable slot (RFC 0021).
  const auto known = [&](DifferenceTerm term) {
    if (!term)
      return true;
    const auto first = constraints.lower_bound({term, std::nullopt});
    return (first != constraints.end() && first->first.first == term) ||
           std::ranges::any_of(constraints, [&](const auto &entry) {
             return entry.first.second == term;
           });
  };
  if (constraints.size() >= MaxTraversalVariables / 2 &&
      (!known(x) || !known(y))) {
    std::set<DifferenceTerm> variables{x, y};
    for (const auto &[pair, value] : constraints) {
      (void)value;
      variables.insert(pair.first);
      variables.insert(pair.second);
    }
    variables.erase(std::nullopt);
    if (variables.size() > MaxTraversalVariables) {
      exhausted = true;
      return false;
    }
  }
  constraints.emplace(key, bound);
  return true;
}

std::optional<std::int64_t>
DifferenceConstraints::bound(DifferenceTerm x, DifferenceTerm y) const {
  // Bellman-Ford on y -> x edges. A negative cycle or an exhausted query
  // supplies no proof, even when an intermediate distance looks sufficient.
  std::map<DifferenceTerm, std::int64_t> distances{{y, 0}};
  std::size_t work = 0;
  for (std::size_t round = 0; round <= MaxTraversalVariables + 1; ++round) {
    bool changed = false;
    for (const auto &[pair, limit] : constraints) {
      if (++work > MaxTraversalSteps) {
        exhausted = true;
        return std::nullopt;
      }
      const auto from = distances.find(pair.second);
      if (from == distances.end())
        continue;
      std::int64_t candidate = 0;
      if (__builtin_add_overflow(from->second, limit, &candidate))
        return std::nullopt;
      const auto [to, inserted] = distances.try_emplace(pair.first, candidate);
      if (inserted || candidate < to->second) {
        to->second = candidate;
        changed = true;
      }
    }
    if (!changed) {
      const auto found = distances.find(x);
      return found == distances.end() ? std::nullopt
                                      : std::optional(found->second);
    }
  }
  return std::nullopt;
}

bool DifferenceConstraints::implies(DifferenceTerm x, DifferenceTerm y,
                                    std::int64_t limit) const {
  const auto value = bound(x, y);
  return value && *value <= limit;
}

void DifferenceConstraints::forget(PlaceId place) {
  std::erase_if(constraints, [&](const auto &entry) {
    return entry.first.first == place || entry.first.second == place;
  });
}

void DifferenceConstraints::assign(PlaceId dest, DifferenceTerm source,
                                   std::int64_t offset) {
  if (source == dest) {
    std::map<Key, std::int64_t> updated;
    for (const auto &[key, value] : constraints) {
      std::int64_t next = value;
      if (key.first != key.second &&
          ((key.first == dest && __builtin_add_overflow(next, offset, &next)) ||
           (key.second == dest && __builtin_sub_overflow(next, offset, &next))))
        continue;
      updated.emplace(key, next);
    }
    constraints = std::move(updated);
    return;
  }
  forget(dest);
  constrain(dest, source, offset);
  if (offset != std::numeric_limits<std::int64_t>::min())
    constrain(source, dest, -offset);
}

bool DifferenceConstraints::join(const DifferenceConstraints &other) {
  bool changed = other.exhausted && !exhausted;
  exhausted |= other.exhausted;
  for (auto it = constraints.begin(); it != constraints.end();) {
    const auto theirs = other.bound(it->first.first, it->first.second);
    if (!theirs) {
      it = constraints.erase(it);
      changed = true;
    } else {
      const auto common = std::max(it->second, *theirs);
      changed |= common != it->second;
      it->second = common;
      ++it;
    }
  }
  return changed;
}

std::optional<IntegerValue> pointerDifference(std::int64_t leftBytes,
                                              std::int64_t rightBytes,
                                              std::int64_t elementBytes,
                                              IntegerType differenceType) {
  if (elementBytes <= 0 || !differenceType.valid() ||
      !differenceType.isSigned || differenceType.isBoolean)
    return std::nullopt;
  std::int64_t bytes = 0;
  if (__builtin_sub_overflow(leftBytes, rightBytes, &bytes) ||
      bytes % elementBytes != 0)
    return std::nullopt;
  const auto value = bytes / elementBytes;
  const auto encoded =
      IntegerValue::ofBits(differenceType, static_cast<std::uint64_t>(value));
  if (encoded.signedValue() != value)
    return std::nullopt;
  return encoded;
}

} // namespace weavec::core
