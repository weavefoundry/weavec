//===- Resource.cpp - Owned resource tracking -----------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Resource.h"

#include <algorithm>
#include <utility>

namespace weavec::core {

void ResourceTracker::hold(PlaceId place, ResourceRecord record) {
  null.erase(place);
  owned.insert_or_assign(place, std::move(record));
}

std::optional<ResourceRecord> ResourceTracker::recordOf(PlaceId place) const {
  const auto it = owned.find(place);
  if (it == owned.end())
    return std::nullopt;
  return it->second;
}

void ResourceTracker::escape(PlaceId place) {
  const auto it = owned.find(place);
  if (it != owned.end())
    it->second.escaped = true;
}

bool ResourceTracker::isEscaped(PlaceId place) const {
  const auto it = owned.find(place);
  return it != owned.end() && it->second.escaped;
}

void ResourceTracker::clear(PlaceId place) {
  owned.erase(place);
}

void ResourceTracker::markNull(PlaceId place) {
  owned.erase(place);
  null.insert(place);
}

void ResourceTracker::forget(PlaceId place) {
  owned.erase(place);
  null.erase(place);
}

bool ResourceTracker::join(const ResourceTracker &other) {
  bool changed = false;
  for (const auto &[place, record] : other.owned) {
    const auto [it, inserted] = owned.try_emplace(place, record);
    if (inserted) {
      changed = true;
      continue;
    }
    ResourceRecord &mine = it->second;
    if (record.escaped && !mine.escaped) {
      mine.escaped = true;
      changed = true;
    }
    // A holder that may point into the resource may not release it.
    if (record.interior && !mine.interior) {
      mine.interior = true;
      changed = true;
    }
    if (mine.family != record.family && !mine.family.empty()) {
      mine.family.clear();
      changed = true;
    }
    // Held when either side's guard holds.
    changed |= mine.guard.join(record.guard);
  }
  const std::size_t before = null.size();
  std::erase_if(
      null, [&other](PlaceId place) { return !other.null.contains(place); });
  changed |= null.size() != before;
  return changed;
}

std::vector<PlaceId> ResourceTracker::learn(PlaceId place,
                                            const ValueFact &fact) {
  std::vector<PlaceId> refuted;
  for (auto it = owned.begin(); it != owned.end();) {
    if (it->second.guard.learn(place, fact) == GuardRefinement::Refuted) {
      refuted.push_back(it->first);
      it = owned.erase(it);
      continue;
    }
    ++it;
  }
  return refuted;
}

void ResourceTracker::dropGuardsOn(PlaceId place) {
  for (auto &[holder, record] : owned)
    record.guard.drop(place);
}

std::vector<PlaceId> ResourceTracker::holders() const {
  std::vector<PlaceId> result;
  result.reserve(owned.size());
  for (const auto &[place, record] : owned)
    result.push_back(place);
  return result;
}

std::vector<PlaceId> ResourceTracker::nullPlaces() const {
  return {null.begin(), null.end()};
}

std::string_view toString(ResourceOrigin origin) noexcept {
  switch (origin) {
  case ResourceOrigin::Allocated:
    return "allocated";
  case ResourceOrigin::Declared:
    return "declared";
  }
  return "<invalid>";
}

} // namespace weavec::core
