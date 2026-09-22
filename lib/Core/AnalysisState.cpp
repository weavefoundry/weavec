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
      guardedBy.erase(it->first);
      releasedBy.erase(it->first);
      replacedBy.erase(it->first);
      it = consumedBy.erase(it);
      continue;
    }
    for (const PlaceId place : it->second)
      std::erase(reinstated, place);
    ++it;
  }
  return reinstated;
}

std::optional<PlaceGuard> PendingOutcome::guardOf(PlaceId place) const {
  std::optional<PlaceGuard> joined;
  for (const auto &[outcome, places] : consumedBy) {
    if (std::ranges::find(places, place) == places.end())
      continue;
    const PlaceGuard *guard = nullptr;
    if (const auto guards = guardedBy.find(outcome);
        guards != guardedBy.end()) {
      for (const auto &[guarded, when] : guards->second) {
        if (guarded == place)
          guard = &when;
      }
    }
    if (guard == nullptr || guard->trivial())
      return std::nullopt;
    if (!joined)
      joined = *guard;
    else
      joined->join(*guard);
    if (joined->trivial())
      return std::nullopt;
  }
  return joined;
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

/// The places of `consumedBy` that every consuming class lists in `per`.
static std::vector<PlaceId>
inEveryConsumer(const PendingOutcome &pending,
                const std::map<Outcome, std::vector<PlaceId>> &per) {
  std::vector<PlaceId> result;
  for (const PlaceId place : pending.places()) {
    bool any = false;
    bool all = true;
    for (const auto &[outcome, consumed] : pending.consumedBy) {
      if (std::ranges::find(consumed, place) == consumed.end())
        continue;
      any = true;
      const auto listed = per.find(outcome);
      all = all && listed != per.end() &&
            std::ranges::find(listed->second, place) != listed->second.end();
    }
    if (any && all)
      result.push_back(place);
  }
  return result;
}

std::vector<PlaceId> PendingOutcome::releasedInAll() const {
  return inEveryConsumer(*this, releasedBy);
}

std::vector<PlaceId> PendingOutcome::replacedInAll() const {
  return inEveryConsumer(*this, replacedBy);
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

bool PendingOutcome::unite(const PendingOutcome &other) {
  // `callee` and `location` name the call in a note: they are the outcome's
  // provenance, not part of what it says about the caller's places. Two
  // different calls can leave every class in the same state, and a test of
  // the result then decides the same thing whichever of them produced it.
  // What a retraction acts on must still match on both: the events it
  // restores and the places the result may be.
  const bool sameCall = location == other.location && callee == other.callee;
  if (localEvents != other.localEvents || returned != other.returned ||
      unheldOnly != other.unheldOnly)
    return false;
  // Two narrowings of one call keep its per-class facts as recorded, so the
  // classes each side kept can be unioned. Two calls have agreed on nothing:
  // a class's null, non-null and integer facts, and its stores, hold on the
  // paths of the call that established them, so taking one side's would
  // claim them on the other's.
  if (!sameCall && (nullOn != other.nullOn || nonNullOn != other.nonNullOn ||
                    factOn != other.factOn || stores != other.stores))
    return false;
  // RFC 0030 §9.1: a class a side's call consumes nothing on has nothing to
  // contribute to it, and must not erase what the other side established.
  // `p = alloc(n); if (!p && n > 0) p = alloc(n);` merges two results, and
  // the retry's null class frees nothing: `n == 0` is the only freeing
  // condition, and `notePendingOutcome` drops a consume whose guard the
  // arguments refute. Dropping the whole outcome there costs the caller the
  // first call's guarded release, and with it every `replaced` derived from
  // it, leaving a bare unguarded consume on every class.
  //
  // The speaking side's consumption stands for such a class, but only where
  // every place it claims there is guarded. The guard is *why* the silent
  // side consumes nothing, so the union claims the consume only on paths
  // that refute it; and §3.1 keeps a guarded consume from settling into a
  // certainty once a test of the result selects the class, so the union can
  // never become a definite finding no path supports.
  const auto silentOn = [](const PendingOutcome &side, Outcome outcome) {
    const auto it = side.consumedBy.find(outcome);
    return it == side.consumedBy.end() || it->second.empty();
  };
  const auto guardsEvery = [](const PendingOutcome &side, Outcome outcome) {
    const auto consumed = side.consumedBy.find(outcome);
    const auto guards = side.guardedBy.find(outcome);
    if (consumed == side.consumedBy.end() || guards == side.guardedBy.end())
      return false;
    return std::ranges::all_of(consumed->second, [&](PlaceId place) {
      return std::ranges::any_of(guards->second, [place](const auto &guard) {
        return guard.first == place && !guard.second.trivial();
      });
    });
  };
  // The classes only one side speaks for, the ones this side is silent on
  // among them, and the ones both speak for.
  std::set<Outcome> oneSided;
  std::set<Outcome> adopted;
  std::set<Outcome> shared;
  const auto classify = [&](Outcome outcome) {
    const bool mineSilent = silentOn(*this, outcome);
    const bool theirsSilent = silentOn(other, outcome);
    if (mineSilent == theirsSilent) {
      if (!mineSilent)
        shared.insert(outcome);
      return;
    }
    if (mineSilent && guardsEvery(other, outcome)) {
      oneSided.insert(outcome);
      adopted.insert(outcome);
    } else if (!mineSilent && guardsEvery(*this, outcome)) {
      oneSided.insert(outcome);
    }
  };
  for (const auto &[outcome, places] : consumedBy)
    classify(outcome);
  for (const auto &[outcome, places] : other.consumedBy)
    classify(outcome);
  // A class both sides speak for was narrowed from equivalent recordings, so
  // it must name the same places on both.
  const auto agrees = [](const auto &mine, const auto &theirs) {
    return std::ranges::all_of(theirs, [&mine](const auto &entry) {
      const auto it = mine.find(entry.first);
      return it == mine.end() || it->second == entry.second;
    });
  };
  const auto without = [&oneSided](const auto &side) {
    std::remove_cvref_t<decltype(side)> kept;
    for (const auto &[outcome, entry] : side) {
      if (!oneSided.contains(outcome))
        kept.emplace(outcome, entry);
    }
    return kept;
  };
  if (sameCall ? !agrees(consumedBy, other.consumedBy)
               : without(consumedBy) != without(other.consumedBy))
    return false;
  if (!agrees(nullOn, other.nullOn) || !agrees(nonNullOn, other.nonNullOn) ||
      !agrees(factOn, other.factOn))
    return false;
  // Across calls a shared class's guards are joined below, so only one call
  // has to agree with itself here.
  if (sameCall && (!agrees(guardedBy, other.guardedBy) ||
                   !agrees(releasedBy, other.releasedBy) ||
                   !agrees(replacedBy, other.replacedBy)))
    return false;
  const auto mineGuards = guardedBy;
  const auto mineReleased = releasedBy;
  const auto mineReplaced = replacedBy;
  const auto merge = [&adopted](auto &mine, const auto &theirs) {
    for (const auto &[outcome, entry] : theirs) {
      const auto [it, inserted] = mine.try_emplace(outcome, entry);
      if (!inserted && adopted.contains(outcome))
        it->second = entry;
    }
    // Nothing of this side's silence survives on a class it hands over.
    for (const Outcome outcome : adopted)
      if (!theirs.contains(outcome))
        mine.erase(outcome);
  };
  merge(consumedBy, other.consumedBy);
  merge(guardedBy, other.guardedBy);
  merge(releasedBy, other.releasedBy);
  merge(replacedBy, other.replacedBy);
  merge(nullOn, other.nullOn);
  merge(nonNullOn, other.nonNullOn);
  merge(factOn, other.factOn);
  // On a class both calls consume, the consume happens when either side's
  // guard holds, so the guards join. A place in `consumedBy` with no
  // `guardedBy` entry is consumed whatever the arguments at that call — the
  // recording drops a guard the arguments already satisfy — so a side that
  // does not name it joins to nothing and the entry goes. What `releasedBy`
  // and `replacedBy` say is a must-fact about the class: what both say.
  if (!sameCall) {
    for (const Outcome outcome : shared) {
      const auto mine = mineGuards.find(outcome);
      const auto theirs = other.guardedBy.find(outcome);
      std::vector<std::pair<PlaceId, PlaceGuard>> joined;
      if (mine != mineGuards.end() && theirs != other.guardedBy.end()) {
        for (const auto &[place, guard] : mine->second) {
          const auto match = std::ranges::find_if(
              theirs->second, [place = place](const auto &entry) {
                return entry.first == place;
              });
          if (match == theirs->second.end())
            continue;
          PlaceGuard both = guard;
          both.join(match->second);
          if (!both.trivial())
            joined.emplace_back(place, std::move(both));
        }
      }
      if (joined.empty())
        guardedBy.erase(outcome);
      else
        guardedBy[outcome] = std::move(joined);
      const auto keepBoth = [&outcome](auto &into, const auto &mineSide,
                                       const auto &theirsSide) {
        const auto a = mineSide.find(outcome);
        const auto b = theirsSide.find(outcome);
        if (a == mineSide.end() || b == theirsSide.end()) {
          into.erase(outcome);
          return;
        }
        std::vector<PlaceId> both;
        for (const PlaceId place : a->second)
          if (std::ranges::find(b->second, place) != b->second.end())
            both.push_back(place);
        if (both.empty())
          into.erase(outcome);
        else
          into[outcome] = std::move(both);
      };
      keepBoth(releasedBy, mineReleased, other.releasedBy);
      keepBoth(replacedBy, mineReplaced, other.replacedBy);
    }
  }
  // A store one side retracted (on none of its classes) is back with the
  // classes it happens on.
  for (const PendingStore &store : other.stores) {
    if (std::ranges::find(stores, store) == stores.end())
      stores.push_back(store);
  }
  // The note can name neither call, so it names no call rather than the
  // wrong one; every site that would add it tests the location first.
  if (!sameCall) {
    location = {};
    callee.clear();
  }
  return true;
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

bool AnalysisState::join(const AnalysisState &other, const PlaceTable *places,
                         bool widenScalars) {
  const auto isNull = [](PlaceId place, const AnalysisState &state) {
    return state.resources.isNull(place) ||
           state.nulls.stateOf(place) == Nullness::Null;
  };
  auto localObjects = heapLocalObjects;
  std::erase_if(localObjects, [&](PlaceId place) {
    return !other.heapLocalObjects.contains(place) && !isNull(place, other);
  });
  for (const auto place : other.heapLocalObjects) {
    if (isNull(place, *this))
      localObjects.insert(place);
  }
  const bool localObjectsChanged = localObjects != heapLocalObjects;
  heapLocalObjects = std::move(localObjects);
  bool spatialChanged = false;
  if (places != nullptr) {
    // RFC 0013: the null branch has no object whose missing spatial fact
    // could contradict the non-null branch. Unknown pointers still weaken.
    const auto absent = [places](PlaceId cell, const AnalysisState &state) {
      const auto isNull = [&state](PlaceId pointer) {
        return state.resources.isNull(pointer) ||
               state.nulls.stateOf(pointer) == Nullness::Null;
      };
      if (isNull(cell))
        return true;
      while (const auto parent = places->parent(cell)) {
        if (places->step(cell) == PathStep::Deref && isNull(*parent))
          return true;
        cell = *parent;
      }
      return false;
    };
    spatialChanged = spatial.joinWithAbsentObjects(
        other.spatial, [&](PlaceId cell) { return absent(cell, *this); },
        [&](PlaceId cell) { return absent(cell, other); });
  } else {
    spatialChanged = spatial.join(other.spatial);
  }
  bool changed =
      spatialChanged || localObjectsChanged || (returned && !other.returned);
  returned = returned && other.returned;
  changed |= moves.join(other.moves);
  changed |= loans.join(other.loans);
  changed |= aliases.join(other.aliases);
  changed |= definiteAliases.intersect(other.definiteAliases);
  changed |= std::erase_if(distinctObjects, [&](const auto &pair) {
               return !other.distinctObjects.contains(pair);
             }) != 0;
  for (const auto &pair : other.testedAliases)
    changed |= testedAliases.insert(pair).second;
  changed |= raw.join(other.raw);
  changed |= resources.join(other.resources);
  changed |= nulls.join(other.nulls);
  changed |= scalars.join(other.scalars, widenScalars);
  changed |= numericWrites.join(other.numericWrites);
  changed |= numericConditions.join(other.numericConditions);
  changed |= !numericConditionsIncomplete && other.numericConditionsIncomplete;
  numericConditionsIncomplete |= other.numericConditionsIncomplete;
  changed |= std::erase_if(numericValues, [&](const auto &entry) {
               const auto found = other.numericValues.find(entry.first);
               return found == other.numericValues.end() ||
                      found->second != entry.second;
             }) != 0;
  changed |= relations.join(other.relations);
  changed |= pointerFacts.join(other.pointerFacts);
  for (auto &[key, range] : filledArrayRanges) {
    const auto found = other.filledArrayRanges.find(key);
    if (found == other.filledArrayRanges.end() ||
        found->second.count != range.count ||
        found->second.storage != range.storage ||
        found->second.bytes != range.bytes || !found->second.definite) {
      changed |= range.definite;
      range.definite = false;
    }
    for (auto it = range.materialized.begin();
         it != range.materialized.end();) {
      if (found == other.filledArrayRanges.end() ||
          !found->second.materialized.contains(*it)) {
        it = range.materialized.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
  }
  for (const auto &[key, range] : other.filledArrayRanges) {
    if (filledArrayRanges.contains(key))
      continue;
    auto copy = range;
    copy.definite = false;
    filledArrayRanges.emplace(key, std::move(copy));
    changed = true;
  }
  for (auto &[key, range] : releasedArrayRanges) {
    const auto found = other.releasedArrayRanges.find(key);
    if (found == other.releasedArrayRanges.end() ||
        found->second.span != range.span ||
        found->second.storage != range.storage) {
      // A range exists only on one path; existing moved cells retain may
      // evidence, but an unvisited cell has no definite traversal proof.
      changed |= range.definite;
      range.definite = false;
      continue;
    }
    if (!found->second.definite && range.definite) {
      range.definite = false;
      changed = true;
    }
    for (auto it = range.materialized.begin();
         it != range.materialized.end();) {
      if (!found->second.materialized.contains(*it)) {
        it = range.materialized.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
  }
  for (const auto &[key, range] : other.releasedArrayRanges) {
    if (releasedArrayRanges.contains(key))
      continue;
    auto copy = range;
    copy.definite = false;
    releasedArrayRanges.emplace(key, std::move(copy));
    changed = true;
  }
  for (auto &[key, range] : arrayRanges) {
    const auto found = other.arrayRanges.find(key);
    if (found == other.arrayRanges.end() ||
        range.destination != found->second.destination ||
        range.source != found->second.source ||
        range.span != found->second.span ||
        range.sourceBegin != found->second.sourceBegin) {
      changed |= range.definite || !range.materialized.empty() ||
                 !range.captured.empty();
      range.definite = false;
      range.materialized.clear();
      range.captured.clear();
      changed |= incompleteHeap.insert(range.destination).second;
      continue;
    }
    const auto &theirs = found->second;
    if (!theirs.definite && range.definite) {
      range.definite = false;
      changed = true;
    }
    if (!theirs.sourceLive && range.sourceLive) {
      range.sourceLive = false;
      changed = true;
    }
    for (auto it = range.captured.begin(); it != range.captured.end();) {
      if (!theirs.captured.contains(*it)) {
        it = range.captured.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
    for (auto it = range.materialized.begin();
         it != range.materialized.end();) {
      if (!theirs.materialized.contains(*it)) {
        it = range.materialized.erase(it);
        changed = true;
      } else {
        ++it;
      }
    }
  }
  for (const auto &[key, range] : other.arrayRanges) {
    if (arrayRanges.contains(key))
      continue;
    auto copy = range;
    copy.definite = false;
    copy.captured.clear();
    copy.materialized.clear();
    arrayRanges.emplace(key, std::move(copy));
    incompleteHeap.insert(range.destination);
    changed = true;
  }
  for (auto it = objectViews.begin(); it != objectViews.end();) {
    const auto found = other.objectViews.find(it->first);
    if (found == other.objectViews.end() || found->second != it->second)
      it = objectViews.erase(it);
    else
      ++it;
  }
  for (auto &[place, targets] : callTargets) {
    const auto it = other.callTargets.find(place);
    changed |= targets.join(it == other.callTargets.end() ? CallTargets::any()
                                                          : it->second);
  }
  for (const auto &[place, targets] : other.callTargets) {
    if (!callTargets.contains(place)) {
      auto joined = targets;
      joined.unknown = true;
      callTargets.emplace(place, std::move(joined));
      changed = true;
    }
  }
  for (const PlaceId root : other.incompleteHeap)
    changed |= incompleteHeap.insert(root).second;
  for (const PlaceId place : other.reinterpreted)
    changed |= reinterpreted.insert(place).second;
  // RFC 0030 §3.1: released on some path; stored since on every path.
  for (const std::uint64_t type : other.releasedTypes)
    changed |= releasedTypes.insert(type).second;
  if (other.releasedUnowned && !releasedUnowned) {
    releasedUnowned = true;
    changed = true;
  }
  for (auto it = storedSinceRelease.begin(); it != storedSinceRelease.end();) {
    if (other.storedSinceRelease.contains(*it)) {
      ++it;
    } else {
      it = storedSinceRelease.erase(it);
      changed = true;
    }
  }
  for (auto it = incoming.begin(); it != incoming.end();) {
    const auto theirs = other.incoming.find(it->first);
    if (theirs == other.incoming.end() || theirs->second != it->second) {
      it = incoming.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  for (auto it = definiteHeapWrites.begin(); it != definiteHeapWrites.end();) {
    if (!other.definiteHeapWrites.contains(*it)) {
      it = definiteHeapWrites.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (const auto &[place, escaped] : other.heapInputEscapes) {
    const auto [it, added] = heapInputEscapes.try_emplace(place, escaped);
    if (added) {
      changed = true;
    } else if (escaped && !it->second) {
      it->second = true;
      changed = true;
    }
  }
  for (const auto &[place, guard] : other.heapWriteGuards) {
    const auto [it, added] = heapWriteGuards.try_emplace(place, guard);
    if (added) {
      changed = true;
    } else {
      // `GuardOn::join` reports exactly whether the guard changed.
      changed |= it->second.join(guard);
    }
  }

  // A pending outcome that is only pending on one incoming path cannot be
  // safely undone, so keep only entries both sides agree on. Two narrowings
  // of one call's outcome (`if (q == NULL) { ... } ... return q;` merges the
  // null edge with the non-null one) are the same outcome with the classes
  // each side kept: their union, class by class (RFC 0006, *Pending
  // outcomes*; RFC 0009, *Guards*).
  for (auto it = pending.begin(); it != pending.end();) {
    const auto theirs = other.pending.find(it->first);
    if (theirs == other.pending.end()) {
      it = pending.erase(it);
      changed = true;
      continue;
    }
    if (theirs->second == it->second) {
      ++it;
      continue;
    }
    if (!it->second.unite(theirs->second)) {
      it = pending.erase(it);
      changed = true;
      continue;
    }
    changed = true;
    ++it;
  }

  // RFC 0030 §5.1: a place only one path gave a value to is not
  // established after the join, so it inherits again.
  if (!established.empty()) {
    const std::size_t before = established.size();
    std::erase_if(established, [&other](PlaceId place) {
      return !std::ranges::binary_search(other.established, place);
    });
    changed = changed || established.size() != before;
  }
  // RFC 0030 §9.1: the place-level guards follow their effects, so they are
  // joined against the consumption as it stands *before* the merge below. A
  // path only one side consumed keeps that side's guard; one both sides
  // consumed keeps what the two agree on, so a second, unguarded consume
  // leaves nothing for a `return` to key on.
  for (auto it = consumedOn.begin(); it != consumedOn.end();) {
    if (!other.consumed.contains(it->first)) {
      ++it;
      continue;
    }
    const auto theirs = other.consumedOn.find(it->first);
    if (theirs == other.consumedOn.end()) {
      it = consumedOn.erase(it);
      changed = true;
      continue;
    }
    if (it->second.join(theirs->second)) {
      changed = true;
      if (it->second.trivial()) {
        it = consumedOn.erase(it);
        continue;
      }
    }
    ++it;
  }
  for (const auto &[path, guard] : other.consumedOn) {
    if (consumed.contains(path))
      continue;
    changed |= consumedOn.try_emplace(path, guard).second;
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

  // Stored on some path (RFC 0010). Both sets are ordered: one merge
  // unless the other side is much smaller.
  if (other.stored.size() * 8 < stored.size()) {
    for (const SummaryPath &path : other.stored)
      changed |= stored.insert(path).second;
  } else {
    auto at = stored.begin();
    for (const SummaryPath &path : other.stored) {
      while (at != stored.end() && *at < path)
        ++at;
      if (at != stored.end() && !(path < *at)) {
        ++at;
        continue;
      }
      at = std::next(stored.insert(at, path));
      changed = true;
    }
  }

  // Overwritten on every path: what the other side did not overwrite goes.
  // A merge of the two ordered sets.
  {
    auto theirs = other.overwritten.begin();
    for (auto it = overwritten.begin(); it != overwritten.end();) {
      while (theirs != other.overwritten.end() && *theirs < *it)
        ++theirs;
      if (theirs == other.overwritten.end() || *it < *theirs) {
        it = overwritten.erase(it);
        changed = true;
      } else {
        ++it;
      }
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
  PlaceGuard guard = pointerFacts;
  for (const auto &[place, fact] : scalars.all()) {
    if (guard.size() >= MaxGuardConjuncts)
      break;
    guard.conditions.emplace(place, fact);
  }
  for (const auto &predicate : numericConditions.integers) {
    const auto implied =
        predicate.evaluate([&](PlaceId place, IntegerType type) {
          const auto fact = scalars.factOf(place);
          return fact ? fact->inType(type) : IntegerRange::full(type);
        });
    if (!implied || !*implied)
      guard.requireInteger(predicate);
  }
  // A dereference-established non-null fact is rarely tested again and
  // would crowd the interface guard out.
  for (const auto &[place, record] : nulls.all()) {
    if (guard.size() >= MaxGuardConjuncts)
      break;
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

static void dropOtherGuardsOn(AnalysisState &state, PlaceId place) {
  for (auto &[result, outcome] : state.pending) {
    (void)result;
    for (auto &[cls, facts] : outcome.factOn) {
      (void)cls;
      std::erase_if(facts,
                    [place](const auto &fact) { return fact.first == place; });
    }
  }
  state.numericConditions.drop(place);
  std::erase_if(state.numericValues, [place](const auto &entry) {
    return entry.first == place || entry.second.dependsOn(place);
  });
  state.pointerFacts.drop(place);
  state.moves.dropGuardsOn(place);
  state.resources.dropGuardsOn(place);
  state.nulls.dropGuardsOn(place);
}

void AnalysisState::dropGuardsOn(PlaceId place) {
  dropOtherGuardsOn(*this, place);
}

void AnalysisState::dropGuardsOn(std::vector<PlaceId> places) {
  if (places.empty())
    return;
  if (places.size() == 1) {
    dropGuardsOn(places.front());
    return;
  }
  std::ranges::sort(places);
  const auto matches = [&](PlaceId place) {
    return std::ranges::binary_search(places, place);
  };
  for (auto &[result, outcome] : pending) {
    (void)result;
    for (auto &[cls, facts] : outcome.factOn) {
      (void)cls;
      std::erase_if(facts,
                    [&](const auto &fact) { return matches(fact.first); });
    }
  }
  numericConditions.dropIf(matches);
  std::erase_if(numericValues, [&](const auto &entry) {
    return matches(entry.first) || entry.second.dependsOnIf(matches);
  });
  pointerFacts.dropIf(matches);
  moves.dropGuardsIf(matches);
  resources.dropGuardsIf(matches);
  nulls.dropGuardsIf(matches);
}

/// Everything `forget` clears about `place` but the guard conjuncts.
static void forgetFacts(AnalysisState &state, PlaceId place) {
  state.numericWrites.insert(place);
  state.moves.reinitialize(place);
  state.aliases.separate(place);
  state.definiteAliases.separate(place);
  std::erase_if(state.testedAliases, [place](const auto &pair) {
    return pair.first == place || pair.second == place;
  });
  std::erase_if(state.distinctObjects, [place](const auto &pair) {
    return pair.first == place || pair.second == place;
  });
  state.loans.dropHolder(place);
  state.loans.release(place);
  state.pending.erase(place);
  state.kinds.erase(place);
  state.raw.clear(place);
  state.resources.forget(place);
  state.nulls.forget(place);
  state.scalars.forget(place);
  state.spatial.forget(place);
  state.relations.forget(place);
  state.callTargets.erase(place);
  state.objectViews.erase(place);
  state.incoming.erase(place);
  state.heapWriteGuards.erase(place);
  state.heapInputEscapes.erase(place);
  state.definiteHeapWrites.erase(place);
  state.heapLocalObjects.erase(place);
  state.incompleteHeap.erase(place);
  state.reinterpreted.erase(place);
  state.storedSinceRelease.erase(place);
}

void AnalysisState::forget(PlaceId place) {
  forgetFacts(*this, place);
  // Pending outputs and the remaining guarded domains still need a scan.
  dropOtherGuardsOn(*this, place);
}

void AnalysisState::forget(std::vector<PlaceId> places) {
  // `forgetFacts` reads no record: erase them in one pass.
  moves.reinitializeAll(places);
  for (const PlaceId place : places)
    forgetFacts(*this, place);
  dropGuardsOn(std::move(places));
}

void AnalysisState::noteRelease(std::uint64_t type, bool owned) {
  releasedTypes.insert(type);
  releasedUnowned = releasedUnowned || !owned;
  storedSinceRelease.clear();
}

} // namespace weavec::core
