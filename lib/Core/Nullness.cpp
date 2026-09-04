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
  // null; the other could hold anything, so refuting the guard says nothing).
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
    if (it->second.otherwiseNonNull) {
      it->second.otherwiseNonNull = false;
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
      joined.otherwiseNonNull = false;
      records.emplace(place, std::move(joined));
      changed = true;
      continue;
    }
    NullRecord &mine = it->second;
    if (mine.state == Nullness::NonNull && theirs.state == Nullness::NonNull)
      continue;
    if (theirs.state == Nullness::NonNull) {
      // `Null` here, non-null there: null exactly when this side's guard
      // holds. `MaybeNull` here keeps its guard and its promise.
      if (mine.state == Nullness::Null) {
        mine.state = Nullness::MaybeNull;
        mine.otherwiseNonNull = true;
        changed = true;
      }
      continue;
    }
    if (mine.state == Nullness::NonNull) {
      // Non-null here, null or maybe there: the other side's record, with
      // the promise that the paths outside its guard (this side) are
      // non-null unless that side already broke it.
      mine = theirs;
      mine.state = Nullness::MaybeNull;
      if (theirs.state == Nullness::Null)
        mine.otherwiseNonNull = true;
      changed = true;
      continue;
    }
    // Both sides may be null: null when either guard holds; the promise
    // survives only if both sides made it (a `Null` side has no non-null
    // paths to promise about, so it keeps the other's promise).
    changed |= mine.guard.join(theirs.guard);
    if (mine.state == Nullness::Null && theirs.state == Nullness::Null)
      continue;
    const bool promise =
        (mine.state == Nullness::Null || mine.otherwiseNonNull) &&
        (theirs.state == Nullness::Null || theirs.otherwiseNonNull);
    if (mine.state != Nullness::MaybeNull) {
      mine.state = Nullness::MaybeNull;
      changed = true;
    }
    if (mine.otherwiseNonNull != promise) {
      mine.otherwiseNonNull = promise;
      changed = true;
    }
  }
  return changed;
}

std::vector<PlaceId> NullTracker::learn(PlaceId place, const ValueFact &fact) {
  std::vector<PlaceId> changed;
  for (auto it = records.begin(); it != records.end();) {
    NullRecord &record = it->second;
    if (record.state == Nullness::NonNull) {
      ++it;
      continue;
    }
    if (record.guard.learn(place, fact) != GuardRefinement::Refuted) {
      ++it;
      continue;
    }
    changed.push_back(it->first);
    if (record.state == Nullness::MaybeNull && record.otherwiseNonNull) {
      record.state = Nullness::NonNull;
      record.guard.conditions.clear();
      record.otherwiseNonNull = false;
      ++it;
      continue;
    }
    it = records.erase(it);
  }
  return changed;
}

void NullTracker::dropGuardsOn(PlaceId place) {
  for (auto &[holder, record] : records)
    record.guard.drop(place);
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
