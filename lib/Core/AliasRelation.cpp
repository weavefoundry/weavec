//===- AliasRelation.cpp - May-alias relation over places -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AliasRelation.h"

#include <algorithm>

namespace weavec::core {

void AliasRelation::relate(PlaceId a, PlaceId b) {
  if (a == b)
    return;
  adjacent[a].insert(b);
  adjacent[b].insert(a);
}

void AliasRelation::unite(PlaceId a, PlaceId b) {
  if (a == b)
    return;
  // Snapshot first: relating mutates the sets being iterated.
  const std::vector<PlaceId> aliasesOfA = members(a);
  const std::vector<PlaceId> aliasesOfB = members(b);
  for (const PlaceId other : aliasesOfB)
    relate(a, other);
  for (const PlaceId other : aliasesOfA)
    relate(b, other);
}

void AliasRelation::separate(PlaceId place) {
  const auto it = adjacent.find(place);
  if (it == adjacent.end())
    return;
  for (const PlaceId other : it->second) {
    const auto otherIt = adjacent.find(other);
    otherIt->second.erase(place);
    if (otherIt->second.empty())
      adjacent.erase(otherIt);
  }
  adjacent.erase(it);
}

bool AliasRelation::mayAlias(PlaceId a, PlaceId b) const noexcept {
  if (a == b)
    return true;
  const auto it = adjacent.find(a);
  return it != adjacent.end() && it->second.contains(b);
}

std::vector<PlaceId> AliasRelation::members(PlaceId place) const {
  std::vector<PlaceId> result{place};
  if (const auto it = adjacent.find(place); it != adjacent.end())
    result.insert(result.end(), it->second.begin(), it->second.end());
  std::ranges::sort(result);
  return result;
}

void AliasRelation::join(const AliasRelation &other) {
  for (const auto &[place, aliases] : other.adjacent) {
    for (const PlaceId alias : aliases)
      relate(place, alias);
  }
}

std::vector<std::pair<PlaceId, PlaceId>> AliasRelation::pairs() const {
  std::vector<std::pair<PlaceId, PlaceId>> result;
  for (const auto &[place, aliases] : adjacent) {
    for (const PlaceId alias : aliases) {
      if (place < alias)
        result.emplace_back(place, alias);
    }
  }
  return result;
}

} // namespace weavec::core
