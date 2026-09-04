//===- AnalysisState.cpp - Per-program-point dataflow state ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AnalysisState.h"

#include <algorithm>
#include <iterator>
#include <utility>

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

/// The places in `facts` for every class of `consumedBy`.
static std::vector<PlaceId>
inAllClasses(const std::map<Outcome, std::vector<PlaceId>> &consumedBy,
             const std::map<Outcome, std::vector<PlaceId>> &facts) {
  std::vector<PlaceId> result;
  bool first = true;
  for (const auto &[outcome, places] : consumedBy) {
    const auto it = facts.find(outcome);
    if (it == facts.end())
      return {};
    std::vector<PlaceId> theirs = it->second;
    std::ranges::sort(theirs);
    if (first) {
      result = std::move(theirs);
      first = false;
      continue;
    }
    std::vector<PlaceId> both;
    std::ranges::set_intersection(result, theirs, std::back_inserter(both));
    result = std::move(both);
    if (result.empty())
      return {};
  }
  return result;
}

std::vector<PlaceId> PendingOutcome::nullInAll() const {
  return inAllClasses(consumedBy, nullOn);
}

std::vector<PlaceId> PendingOutcome::nonNullInAll() const {
  return inAllClasses(consumedBy, nonNullOn);
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
  changed |= resources.join(other.resources);
  changed |= nulls.join(other.nulls);

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

  // Overwritten on every path: what the other side did not overwrite goes.
  for (auto it = overwritten.begin(); it != overwritten.end();) {
    if (!other.overwritten.contains(*it)) {
      it = overwritten.erase(it);
      changed = true;
    } else {
      ++it;
    }
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

bool AnalysisState::isOverwritten(const SummaryPath &path) const {
  return std::ranges::any_of(overwritten, [&path](const SummaryPath &other) {
    if (other == path)
      return true;
    if (!other.isProperPrefixOf(path))
      return false;
    // Overwriting an object overwrites its fields, not what its pointers
    // point to: `*b = t` replaces `b->data`, `p = q` replaces nothing below
    // `*p`.
    return std::none_of(
        std::next(path.steps.begin(),
                  static_cast<std::ptrdiff_t>(other.steps.size())),
        path.steps.end(),
        [](const PathElem &elem) { return elem.step == PathStep::Deref; });
  });
}

void AnalysisState::forget(PlaceId place) {
  moves.reinitialize(place);
  aliases.separate(place);
  loans.dropHolder(place);
  loans.release(place);
  pending.erase(place);
  kinds.erase(place);
  raw.clear(place);
  resources.forget(place);
  nulls.forget(place);
}

} // namespace weavec::core
