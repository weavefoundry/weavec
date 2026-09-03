//===- AnalysisState.cpp - Per-program-point dataflow state ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AnalysisState.h"

#include <algorithm>

namespace weavec::core {

std::vector<PlaceId> PendingOutcome::places() const {
  std::vector<PlaceId> result;
  for (const auto &[outcome, places] : consumedBy) {
    for (const PlaceId place : places) {
      if (!std::ranges::binary_search(result, place))
        result.insert(std::ranges::upper_bound(result, place), place);
    }
  }
  return result;
}

std::vector<PlaceId> PendingOutcome::select(const std::set<Outcome> &selected) {
  const bool feasible =
      std::ranges::any_of(consumedBy, [&selected](const auto &entry) {
        return selected.contains(entry.first);
      });
  if (!feasible)
    return {};
  std::vector<PlaceId> reinstated = places();
  for (auto it = consumedBy.begin(); it != consumedBy.end();) {
    if (!selected.contains(it->first)) {
      it = consumedBy.erase(it);
      continue;
    }
    for (const PlaceId place : it->second)
      std::erase(reinstated, place);
    ++it;
  }
  return reinstated;
}

bool PendingOutcome::settled() const {
  const std::vector<PlaceId> all = places();
  return std::ranges::all_of(consumedBy, [&all](const auto &entry) {
    return std::ranges::all_of(all, [&entry](PlaceId place) {
      return std::ranges::find(entry.second, place) != entry.second.end();
    });
  });
}

bool AnalysisState::join(const AnalysisState &other) {
  bool changed = false;
  changed |= moves.join(other.moves);
  changed |= loans.join(other.loans);
  changed |= aliases.join(other.aliases);
  changed |= raw.join(other.raw);

  // A pending outcome that is only pending on one incoming path cannot be
  // safely undone, so keep only entries both sides agree on.
  for (auto it = pending.begin(); it != pending.end();) {
    const auto theirs = other.pending.find(it->first);
    if (theirs == other.pending.end() || theirs->second != it->second) {
      it = pending.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  for (const auto &[path, effect] : other.consumed) {
    auto [it, inserted] = consumed.try_emplace(path, effect);
    if (inserted) {
      changed = true;
      continue;
    }
    const PlaceEffect before = it->second;
    it->second.join(effect);
    changed |= it->second != before;
  }

  for (const auto &[place, kind] : other.kinds) {
    auto [it, inserted] = kinds.try_emplace(place, kind);
    if (inserted) {
      changed = true;
      continue;
    }
    const OwnershipKind joined = core::join(it->second, kind);
    if (joined != it->second) {
      it->second = joined;
      changed = true;
    }
  }
  return changed;
}

OwnershipKind AnalysisState::kindOf(PlaceId place) const noexcept {
  const auto it = kinds.find(place);
  return it == kinds.end() ? OwnershipKind::Unknown : it->second;
}

void AnalysisState::forget(PlaceId place) {
  moves.reinitialize(place);
  aliases.separate(place);
  loans.dropHolder(place);
  loans.release(place);
  pending.erase(place);
  kinds.erase(place);
  raw.clear(place);
}

} // namespace weavec::core
