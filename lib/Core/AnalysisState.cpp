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

std::vector<PlaceId> PendingOutcome::nullInAll() const {
  return inAllClasses(consumedBy, nullOn);
}

std::vector<PlaceId> PendingOutcome::nonNullInAll() const {
  return inAllClasses(consumedBy, nonNullOn);
}

std::vector<std::pair<PlaceId, InitializedRange>>
PendingOutcome::initializedInAll() const {
  std::vector<std::pair<PlaceId, InitializedRange>> result;
  bool first = true;
  for (const auto &[outcome, consumed] : consumedBy) {
    (void)consumed;
    const auto found = initializedOn.find(outcome);
    if (found == initializedOn.end())
      return {};
    if (first) {
      result = found->second;
      first = false;
    } else {
      std::erase_if(result, [&](const auto &fact) {
        return std::ranges::find(found->second, fact) == found->second.end();
      });
    }
  }
  return result;
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
  if (location != other.location || callee != other.callee ||
      returned != other.returned || unheldOnly != other.unheldOnly)
    return false;
  for (const auto &[outcome, consumed] : consumedBy) {
    (void)consumed;
    if (!other.consumedBy.contains(outcome))
      continue;
    const auto ours = initializedOn.find(outcome);
    const auto theirs = other.initializedOn.find(outcome);
    if ((ours == initializedOn.end()) !=
            (theirs == other.initializedOn.end()) ||
        (ours != initializedOn.end() && ours->second != theirs->second))
      return false;
  }
  // A class both sides kept was narrowed from the same recording: it must
  // say the same on both.
  const auto agrees = [](const auto &mine, const auto &theirs) {
    return std::ranges::all_of(theirs, [&mine](const auto &entry) {
      const auto it = mine.find(entry.first);
      return it == mine.end() || it->second == entry.second;
    });
  };
  if (!agrees(consumedBy, other.consumedBy) ||
      !agrees(guardedBy, other.guardedBy) || !agrees(nullOn, other.nullOn) ||
      !agrees(nonNullOn, other.nonNullOn) || !agrees(factOn, other.factOn))
    return false;
  const auto merge = [](auto &mine, const auto &theirs) {
    for (const auto &[outcome, entry] : theirs)
      mine.try_emplace(outcome, entry);
  };
  merge(consumedBy, other.consumedBy);
  merge(guardedBy, other.guardedBy);
  merge(nullOn, other.nullOn);
  merge(nonNullOn, other.nonNullOn);
  merge(factOn, other.factOn);
  merge(initializedOn, other.initializedOn);
  // A store one side retracted (on none of its classes) is back with the
  // classes it happens on.
  for (const PendingStore &store : other.stores) {
    if (std::ranges::find(stores, store) == stores.end())
      stores.push_back(store);
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
  const auto leftSafetyGuard = safety ? pathGuard() : PlaceGuard{};
  const auto rightSafetyGuard = other.safety ? other.pathGuard() : PlaceGuard{};
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
  changed |=
      relations.join(other.relations, safety && other.safety, widenScalars);
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
      const auto before = it->second;
      it->second.join(guard);
      changed |= before != it->second;
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
  if (safety && other.safety) {
    changed |= safety->join(*other.safety, leftSafetyGuard, rightSafetyGuard);
  } else if (safety) {
    changed |= safety->join(SafetyState{});
  } else if (other.safety) {
    safety.emplace();
    safety->join(*other.safety);
    changed = true;
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
  if (state.safety)
    for (auto &[result, outcome] : state.pending) {
      (void)result;
      for (auto &[cls, facts] : outcome.initializedOn) {
        (void)cls;
        std::erase_if(facts, [&](const auto &fact) {
          return fact.first == place || fact.second.begin.place == place ||
                 fact.second.end.place == place ||
                 fact.second.when.dependsOn(place);
        });
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
  // RFC 0018: a write cannot reinterpret an earlier initialized interval
  // using the new value of its index or count (including callee outputs).
  if (safety)
    safety->forgetDependency(place);
  dropOtherGuardsOn(*this, place);
}

void AnalysisState::forgetZeroedMemory() {
  if (!safety)
    return;
  safety->forgetZeros();
  for (auto &[result, outcome] : pending) {
    (void)result;
    for (auto &[cls, facts] : outcome.initializedOn) {
      (void)cls;
      std::erase_if(facts, [](const auto &fact) {
        return fact.second.zeroed || fact.second.terminatedWithin;
      });
    }
  }
}

void AnalysisState::forget(PlaceId place) {
  if (safety)
    safety->forget(place);
  numericWrites.insert(place);
  moves.reinitialize(place);
  aliases.separate(place);
  definiteAliases.separate(place);
  std::erase_if(distinctObjects, [place](const auto &pair) {
    return pair.first == place || pair.second == place;
  });
  loans.dropHolder(place);
  loans.release(place);
  pending.erase(place);
  kinds.erase(place);
  raw.clear(place);
  resources.forget(place);
  nulls.forget(place);
  scalars.forget(place);
  spatial.forget(place);
  relations.forget(place);
  callTargets.erase(place);
  objectViews.erase(place);
  incoming.erase(place);
  heapWriteGuards.erase(place);
  heapInputEscapes.erase(place);
  definiteHeapWrites.erase(place);
  heapLocalObjects.erase(place);
  incompleteHeap.erase(place);
  // RFC 0020: safety->forget above already invalidated safety dependencies.
  // Pending outputs and the remaining guarded domains still need a scan.
  dropOtherGuardsOn(*this, place);
}

} // namespace weavec::core
