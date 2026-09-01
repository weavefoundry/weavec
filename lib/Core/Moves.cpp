//===- Moves.cpp - Move / deinitialization tracking -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Moves.h"

#include <utility>

namespace weavec::core {

std::optional<MoveRecord> MoveTracker::markMoved(PlaceId place,
                                                 MoveReason reason,
                                                 SourceLocation location,
                                                 std::optional<PlaceId> via) {
  MoveRecord record{
      .reason = reason, .location = std::move(location), .via = via};
  auto [it, inserted] = moved.try_emplace(place, record);
  if (inserted)
    return std::nullopt;
  // Already moved: report the earlier move but keep the original record so
  // later diagnostics point at the first offending site.
  return it->second;
}

void MoveTracker::reinitialize(PlaceId place) {
  moved.erase(place);
}

std::optional<MoveRecord> MoveTracker::movedAt(PlaceId place) const {
  const auto it = moved.find(place);
  if (it == moved.end())
    return std::nullopt;
  return it->second;
}

void MoveTracker::join(const MoveTracker &other) {
  for (const auto &[place, record] : other.moved)
    moved.try_emplace(place, record);
}

std::vector<PlaceId> MoveTracker::movedPlaces() const {
  std::vector<PlaceId> result;
  result.reserve(moved.size());
  for (const auto &[place, record] : moved)
    result.push_back(place);
  return result;
}

} // namespace weavec::core
