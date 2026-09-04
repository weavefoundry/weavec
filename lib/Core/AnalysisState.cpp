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

std::vector<std::pair<PlaceId, ValueFact>> PendingOutcome::factsInAll() const {
  std::map<PlaceId, ValueFact> joined;
  bool first = true;
  for (const auto &[outcome, places] : consumedBy) {
    const auto it = factOn.find(outcome);
    if (it == factOn.end())
      return {};
    std::map<PlaceId, ValueFact> theirs(it->second.begin(), it->second.end());
    if (first) {
      joined = std::move(theirs);
      first = false;
      continue;
    }
    for (auto mine = joined.begin(); mine != joined.end();) {
      const auto theirFact = theirs.find(mine->first);
      if (theirFact == theirs.end()) {
        mine = joined.erase(mine);
        continue;
      }
      mine->second.join(theirFact->second);
      ++mine;
    }
    if (joined.empty())
      return {};
  }
  std::vector<std::pair<PlaceId, ValueFact>> result;
  for (const auto &[place, fact] : joined) {
    if (!fact.trivial())
      result.emplace_back(place, fact);
  }
  return result;
}

std::vector<PendingOutcome::PendingStore> PendingOutcome::retractStores() {
  OutcomeSet remaining;
  for (const auto &[outcome, places] : consumedBy)
    remaining.insert(outcome);
  std::vector<PendingStore> retracted;
  std::erase_if(stores, [&](const PendingStore &store) {
    if (!(store.on & remaining).empty())
      return false;
    retracted.push_back(store);
    return true;
  });
  return retracted;
}

bool PendingOutcome::settled() const {
  const std::vector<PlaceId> all = places();
  OutcomeSet remaining;
  for (const auto &[outcome, places] : consumedBy)
    remaining.insert(outcome);
  const bool storesSettled =
      std::ranges::all_of(stores, [remaining](const PendingStore &store) {
        return store.on.containsAll(remaining);
      });
  return storesSettled &&
         std::ranges::all_of(consumedBy, [&all](const auto &entry) {
           return std::ranges::all_of(all, [&entry](PlaceId place) {
             return std::ranges::find(entry.second, place) !=
                    entry.second.end();
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
  changed |= scalars.join(other.scalars);

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

  // Stored on some path (RFC 0010).
  for (const SummaryPath &path : other.stored)
    changed |= stored.insert(path).second;

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

std::optional<ValueFact> AnalysisState::factOf(PlaceId place) const {
  if (const auto fact = scalars.factOf(place))
    return fact;
  const auto nullness = nulls.stateOf(place);
  if (!nullness)
    return std::nullopt;
  switch (*nullness) {
  case Nullness::Null:
    return ValueFact::of(Outcome::Null);
  case Nullness::NonNull:
    return ValueFact::of(Outcome::NonNull);
  case Nullness::MaybeNull:
    return std::nullopt;
  }
  return std::nullopt;
}

PlaceGuard AnalysisState::pathGuard() const {
  PlaceGuard guard;
  for (const auto &[place, fact] : scalars.all()) {
    if (guard.conditions.size() >= MaxGuardConjuncts)
      return guard;
    guard.conditions.emplace(place, fact);
  }
  // A definite null, however learnt, and a non-null established by a test
  // are what a later test can contradict; a non-null from a dereference is
  // rarely tested again and would crowd the guard out.
  for (const auto &[place, record] : nulls.all()) {
    if (guard.conditions.size() >= MaxGuardConjuncts)
      return guard;
    if (record.state == Nullness::Null)
      guard.conditions.emplace(place, ValueFact::of(Outcome::Null));
    else if (record.state == Nullness::NonNull &&
             record.reason == NullReason::Tested)
      guard.conditions.emplace(place, ValueFact::of(Outcome::NonNull));
  }
  return guard;
}

AnalysisState::Learned AnalysisState::learn(PlaceId place,
                                            const ValueFact &fact) {
  Learned learned;
  learned.reinstated = moves.learn(place, fact);
  learned.cleared = resources.learn(place, fact);
  learned.nullChanged = nulls.learn(place, fact);
  return learned;
}

void AnalysisState::dropGuardsOn(PlaceId place) {
  moves.dropGuardsOn(place);
  resources.dropGuardsOn(place);
  nulls.dropGuardsOn(place);
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
  scalars.forget(place);
  dropGuardsOn(place);
}

} // namespace weavec::core
