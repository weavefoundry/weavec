//===- Nullness.cpp - May-null / non-null facts per place -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Nullness.h"

#include <utility>

namespace weavec::core {

void NullTracker::set(PlaceId place, NullRecord record) {
  records.insert_or_assign(place, std::move(record));
}

std::optional<NullRecord> NullTracker::recordOf(PlaceId place) const {
  const auto it = records.find(place);
  if (it == records.end())
    return std::nullopt;
  return it->second;
}

std::optional<Nullness> NullTracker::stateOf(PlaceId place) const {
  const auto it = records.find(place);
  if (it == records.end())
    return std::nullopt;
  return it->second.state;
}

void NullTracker::forget(PlaceId place) {
  records.erase(place);
}

bool NullTracker::join(const NullTracker &other) {
  bool changed = false;
  // A fact on this side only: the other path knows nothing, so a `NonNull`
  // fact is lost and a `Null` one weakens to `MaybeNull` (some path has a
  // null; the other could hold anything).
  for (auto it = records.begin(); it != records.end();) {
    if (other.records.contains(it->first)) {
      ++it;
      continue;
    }
    if (it->second.state == Nullness::NonNull) {
      it = records.erase(it);
      changed = true;
      continue;
    }
    if (it->second.state == Nullness::Null) {
      it->second.state = Nullness::MaybeNull;
      changed = true;
    }
    ++it;
  }
  for (const auto &[place, theirs] : other.records) {
    const auto it = records.find(place);
    if (it == records.end()) {
      // No fact here: `Null` or `MaybeNull` there is `MaybeNull` (some path
      // has a null), `NonNull` there is still no fact.
      if (theirs.state == Nullness::NonNull)
        continue;
      NullRecord joined = theirs;
      joined.state = Nullness::MaybeNull;
      records.emplace(place, std::move(joined));
      changed = true;
      continue;
    }
    NullRecord &mine = it->second;
    if (mine.state == theirs.state)
      continue;
    if (mine.state == Nullness::MaybeNull)
      continue;
    // Any disagreement is `MaybeNull`; keep the record that said null.
    if (mine.state == Nullness::NonNull)
      mine = theirs;
    mine.state = Nullness::MaybeNull;
    changed = true;
  }
  return changed;
}

std::vector<PlaceId> NullTracker::places() const {
  std::vector<PlaceId> result;
  result.reserve(records.size());
  for (const auto &[place, record] : records)
    result.push_back(place);
  return result;
}

std::string_view toString(Nullness state) noexcept {
  switch (state) {
  case Nullness::Null:
    return "null";
  case Nullness::MaybeNull:
    return "maybe-null";
  case Nullness::NonNull:
    return "nonnull";
  }
  return "<invalid>";
}

std::string_view toString(NullReason reason) noexcept {
  switch (reason) {
  case NullReason::AssignedNull:
    return "assigned-null";
  case NullReason::CalleeResult:
    return "callee-result";
  case NullReason::CalleeStore:
    return "callee-store";
  case NullReason::Tested:
    return "tested";
  case NullReason::Declared:
    return "declared";
  case NullReason::Dereferenced:
    return "dereferenced";
  }
  return "<invalid>";
}

} // namespace weavec::core
