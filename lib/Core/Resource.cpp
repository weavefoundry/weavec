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

void ResourceTracker::unescape(PlaceId place) {
  const auto it = owned.find(place);
  if (it != owned.end())
    it->second.escaped = false;
}

bool ResourceTracker::isEscaped(PlaceId place) const {
  const auto it = owned.find(place);
  return it != owned.end() && it->second.escaped;
}

void ResourceTracker::clear(PlaceId place) {
  owned.erase(place);
}

ResourceRecord ResourceTracker::retain(PlaceId place, std::string countField,
                                       SourceLocation location) {
  null.erase(place);
  const auto it = owned.find(place);
  if (it == owned.end()) {
    ResourceRecord record{.origin = ResourceOrigin::Retained,
                          .location = std::move(location),
                          .shares = 1,
                          .countField = std::move(countField)};
    owned.emplace(place, record);
    return record;
  }
  ResourceRecord &record = it->second;
  ++record.shares;
  if (record.countField.empty())
    record.countField = std::move(countField);
  else if (record.countField != countField)
    record.countField.clear();
  return record;
}

std::uint32_t ResourceTracker::release(PlaceId place) {
  const auto it = owned.find(place);
  if (it == owned.end())
    return 0;
  if (it->second.shares > 1)
    return --it->second.shares;
  owned.erase(it);
  return 0;
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
    // A retained share has no family of its own (RFC 0010): it neither
    // contradicts nor supplies one, except that the owned side's is kept.
    const bool retainedVsOwned = mine.origin != record.origin &&
                                 (mine.origin == ResourceOrigin::Retained ||
                                  record.origin == ResourceOrigin::Retained);
    if (retainedVsOwned) {
      if (mine.origin == ResourceOrigin::Retained &&
          mine.family != record.family) {
        mine.family = record.family;
        changed = true;
      }
    } else if (mine.family != record.family && !mine.family.empty()) {
      mine.family.clear();
      changed = true;
    }
    // The smaller count: a release is then treated as the last one on the
    // side with more, which reports a use after it rather than missing one
    // (RFC 0010, *Bugs deliberately not caught*).
    if (record.shares < mine.shares) {
      mine.shares = record.shares;
      changed = true;
    }
    if (mine.countField != record.countField && !mine.countField.empty()) {
      mine.countField.clear();
      changed = true;
    }
    // A retained share on one side and an owned resource on the other: the
    // record is the owned one (releasing its last share kills the holder),
    // the more reporting direction.
    if (mine.origin == ResourceOrigin::Retained &&
        record.origin != ResourceOrigin::Retained) {
      mine.origin = record.origin;
      mine.location = record.location;
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
  case ResourceOrigin::Retained:
    return "retained";
  }
  return "<invalid>";
}

} // namespace weavec::core
