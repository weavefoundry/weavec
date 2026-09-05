//===- Summary.cpp - Function summaries for signature inference -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Summary.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace weavec::core {

SummaryPath SummaryPath::deref() const {
  SummaryPath result = *this;
  result.steps.push_back(PathElem{.step = PathStep::Deref, .field = {}});
  return result;
}

SummaryPath SummaryPath::field(std::string_view name) const {
  SummaryPath result = *this;
  result.steps.push_back(
      PathElem{.step = PathStep::Field, .field = std::string(name)});
  return result;
}

SummaryPath SummaryPath::indexed() const {
  // Mirrors `PlaceTable::index`: indexing an index or a dereference
  // collapses onto it.
  if (!steps.empty() && (steps.back().step == PathStep::Index ||
                         steps.back().step == PathStep::Deref))
    return *this;
  SummaryPath result = *this;
  result.steps.push_back(PathElem{.step = PathStep::Index, .field = {}});
  return result;
}

bool SummaryPath::isProperPrefixOf(const SummaryPath &other) const {
  if (root != other.root || index != other.index ||
      steps.size() >= other.steps.size())
    return false;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (steps[i] != other.steps[i])
      return false;
  }
  return true;
}

bool SummaryPath::hasDeref() const noexcept {
  return std::ranges::any_of(
      steps, [](const PathElem &elem) { return elem.step == PathStep::Deref; });
}

std::string SummaryPath::toString(std::string_view rootName) const {
  std::string name(rootName);
  std::size_t i = 0;
  while (i < steps.size()) {
    switch (steps[i].step) {
    case PathStep::Deref:
      // `(*p).f` is spelled `p->f`; a trailing or non-field-followed deref
      // is spelled `*p`.
      if (i + 1 < steps.size() && steps[i + 1].step == PathStep::Field) {
        name += "->" + steps[i + 1].field;
        i += 2;
        continue;
      }
      name.insert(0, 1, '*');
      break;
    case PathStep::Field:
      name += "." + steps[i].field;
      break;
    case PathStep::Index:
      name += "[*]";
      break;
    }
    ++i;
  }
  return name;
}

void PlaceEffect::join(const PlaceEffect &other) {
  const bool wasConsumed = consumed();
  read = read || other.read;
  written = written || other.written;
  freed = freed || other.freed;
  moved = moved || other.moved;
  escaped = escaped || other.escaped;
  if (!other.consumed())
    return;
  if (!wasConsumed) {
    family = other.family;
    replaced = other.replaced;
    element = other.element;
    share = other.share;
    when = other.when;
    at = other.at;
    return;
  }
  if (family != other.family)
    family.clear();
  // Released at two different offsets: the caller cannot compose either.
  at.join(other.at);
  // A side that may leave the consumed value in place makes the join so; a
  // side that consumed the whole pointee makes every later access a use.
  replaced = replaced && other.replaced;
  element = element && other.element;
  // A side that frees the object outright makes the join a plain free: the
  // caller's other shares are then dead too (RFC 0010).
  share = share && other.share;
  // The consume happens when either side's guard holds (RFC 0009).
  when.join(other.when);
}

PlaceEffect FunctionSummary::effectOf(const SummaryPath &path) const {
  const auto it = effects.find(path);
  return it == effects.end() ? PlaceEffect{} : it->second;
}

void FunctionSummary::addEffect(SummaryPath path, const PlaceEffect &effect) {
  if (effect.empty())
    return;
  effects[std::move(path)].join(effect);
}

void FunctionSummary::addStore(Store store) {
  const auto same = std::ranges::find_if(stores, [&store](const Store &s) {
    return s.dest == store.dest && s.value.sameValueAs(store.value);
  });
  if (same == stores.end()) {
    stores.insert(std::move(store));
    return;
  }
  if (same->value.when == store.value.when)
    return;
  Store joined = *same;
  joined.value.when.join(store.value.when);
  stores.erase(same);
  stores.insert(std::move(joined));
}

void FunctionSummary::addReturn(ValueSource source) {
  const auto same =
      std::ranges::find_if(returns, [&source](const ValueSource &s) {
        return s.sameValueAs(source);
      });
  if (same == returns.end()) {
    returns.insert(std::move(source));
    return;
  }
  if (same->when == source.when)
    return;
  ValueSource joined = *same;
  joined.when.join(source.when);
  returns.erase(same);
  returns.insert(std::move(joined));
}

void FunctionSummary::addRequirement(std::uint32_t param,
                                     ExtentRequirement requirement) {
  std::set<ExtentRequirement> &mine = requiresExtent[param];
  const auto same =
      std::ranges::find_if(mine, [&requirement](const ExtentRequirement &r) {
        return r.need == requirement.need;
      });
  if (same == mine.end()) {
    mine.insert(std::move(requirement));
    return;
  }
  if (same->when == requirement.when)
    return;
  ExtentRequirement joined = *same;
  joined.when.join(requirement.when);
  mine.erase(same);
  mine.insert(std::move(joined));
}

bool FunctionSummary::returnsKind(ValueSource::Kind kind) const noexcept {
  return std::ranges::any_of(returns, [kind](const ValueSource &source) {
    return source.kind == kind;
  });
}

void FunctionSummary::eraseReturns(ValueSource::Kind kind) {
  std::erase_if(returns, [kind](const ValueSource &source) {
    return source.kind == kind;
  });
}

bool FunctionSummary::consumes(std::uint32_t param) const {
  return effectOf(SummaryPath::param(param)).consumed();
}

bool FunctionSummary::frees(std::uint32_t param) const {
  return effectOf(SummaryPath::param(param)).freed;
}

std::optional<BorrowKind>
FunctionSummary::borrowKind(std::uint32_t param) const {
  const SummaryPath pointee = SummaryPath::param(param).deref();
  bool read = false;
  bool mutated = false;
  for (const auto &[path, effect] : effects) {
    if (path == pointee || pointee.isProperPrefixOf(path)) {
      read = read || effect.read;
      mutated = mutated || effect.mutates();
    }
  }
  // Handing out a pointer into the pointee (returned or stored elsewhere) is a
  // shared use of it even when nothing was read through it. A copy of the
  // parameter at a field offset (`&n->v`, RFC 0011) points into the pointee
  // as much as a borrow of `param i *.v` did.
  const auto usesPointee = [&](const ValueSource &value) {
    if (value.kind != ValueSource::Kind::Copy &&
        value.kind != ValueSource::Kind::Borrow)
      return false;
    if (!value.path)
      return false;
    if (*value.path == pointee || pointee.isProperPrefixOf(*value.path))
      return true;
    return value.kind == ValueSource::Kind::Copy &&
           *value.path == SummaryPath::param(param) && value.offset.isField();
  };
  for (const Store &store : stores) {
    if (store.dest == pointee || pointee.isProperPrefixOf(store.dest))
      mutated = true;
    read = read || usesPointee(store.value);
  }
  for (const ValueSource &value : returns)
    read = read || usesPointee(value);
  if (mutated)
    return BorrowKind::Mutable;
  if (read)
    return BorrowKind::Shared;
  return std::nullopt;
}

OwnershipKind FunctionSummary::inferredKind(std::uint32_t param) const {
  if (consumes(param))
    return OwnershipKind::Owned;
  if (const auto borrow = borrowKind(param)) {
    return *borrow == BorrowKind::Mutable ? OwnershipKind::Mutable
                                          : OwnershipKind::Shared;
  }
  return OwnershipKind::Unknown;
}

OwnershipKind FunctionSummary::inferredReturnKind() const {
  if (returnsKind(ValueSource::Kind::Raw))
    return OwnershipKind::Raw;
  OwnershipKind result = OwnershipKind::Unknown;
  for (const ValueSource &source : returns) {
    switch (source.kind) {
    case ValueSource::Kind::Raw:
      break;
    case ValueSource::Kind::Fresh:
      result = core::join(result, OwnershipKind::Owned);
      break;
    case ValueSource::Kind::Borrow:
    case ValueSource::Kind::Copy:
      // A borrow whose mutability the signature does not fix is reported as
      // shared, the weaker claim.
      result = core::join(result, OwnershipKind::Shared);
      break;
    case ValueSource::Kind::Null:
      break;
    case ValueSource::Kind::Unknown:
      return OwnershipKind::Unknown;
    }
  }
  return result == OwnershipKind::Raw ? OwnershipKind::Unknown : result;
}

bool FunctionSummary::returnsFresh() const noexcept {
  return std::ranges::any_of(returns, &ValueSource::isFresh);
}

bool FunctionSummary::returnsOnlyFresh() const noexcept {
  return returnsFresh() &&
         std::ranges::all_of(returns, [](const ValueSource &source) {
           return source.isFresh() || source.kind == ValueSource::Kind::Null;
         });
}

std::string FunctionSummary::freshReturnFamily() const {
  std::optional<std::string> family;
  for (const ValueSource &source : returns) {
    if (!source.isFresh())
      continue;
    if (family && *family != source.family)
      return {};
    family = source.family;
  }
  return family.value_or(std::string{});
}

void FunctionSummary::eraseFreshReturns() {
  std::erase_if(returns,
                [](const ValueSource &source) { return source.isFresh(); });
}

bool FunctionSummary::mayReturnNull() const noexcept {
  return returnsKind(ValueSource::Kind::Null);
}

void FunctionSummary::addOutcome(Outcome outcome, const SummaryPath &path,
                                 const PlaceEffect &effect) {
  OutcomeEffects &perClass = outcomes[outcome];
  if (!effect.empty())
    perClass[path].join(effect);
}

std::set<SummaryPath> FunctionSummary::storeDestinations() const {
  std::set<SummaryPath> result;
  for (const Store &store : stores)
    result.insert(store.dest);
  return result;
}

std::set<SummaryPath> FunctionSummary::storesOnClass(Outcome outcome) const {
  if (storesOn.empty() || !outcomes.contains(outcome))
    return storeDestinations();
  const auto it = storesOn.find(outcome);
  return it == storesOn.end() ? std::set<SummaryPath>{} : it->second;
}

void FunctionSummary::normalizeStoresOn() {
  if (storesOn.empty())
    return;
  if (outcomes.empty() || stores.empty()) {
    storesOn.clear();
    return;
  }
  const std::set<SummaryPath> all = storeDestinations();
  const bool uniform = std::ranges::all_of(outcomes, [&](const auto &entry) {
    const auto it = storesOn.find(entry.first);
    return it != storesOn.end() && it->second == all;
  });
  if (uniform)
    storesOn.clear();
}

bool FunctionSummary::retains(std::uint32_t param) const {
  const SummaryPath pointee = SummaryPath::param(param).deref();
  return std::ranges::any_of(increments, [&pointee](const SummaryPath &path) {
    return path == pointee || pointee.isProperPrefixOf(path);
  });
}

bool FunctionSummary::consumesUnconditionally(const SummaryPath &path) const {
  // Without classes the one effect speaks: a consume under a guard (RFC
  // 0009) happens only for some arguments, and is not unconditional.
  if (outcomes.empty()) {
    const auto it = effects.find(path);
    return it == effects.end() || it->second.when.trivial();
  }
  return std::ranges::all_of(outcomes, [&path](const auto &entry) {
    const auto it = entry.second.find(path);
    return it != entry.second.end() && it->second.consumed() &&
           it->second.when.trivial();
  });
}

void FunctionSummary::join(const FunctionSummary &other) {
  // The empty summary is the bottom of the lattice (a join of candidates
  // starts from it): the other side's classes are the answer.
  const bool wasEmpty = empty();
  // What this side stored per class, read before its stores absorb the
  // other side's (RFC 0010, *Per-outcome stores*).
  std::map<Outcome, std::set<SummaryPath>> mineStoresOn;
  for (const auto &[outcome, perClass] : outcomes)
    mineStoresOn[outcome] = storesOnClass(outcome);
  for (const auto &[path, effect] : other.effects)
    addEffect(path, effect);
  for (const Store &store : other.stores)
    addStore(store);
  for (const ValueSource &source : other.returns)
    addReturn(source);
  // A parameter some candidate dereferences must not be null (RFC 0008).
  requiresNonNull.insert(other.requiresNonNull.begin(),
                         other.requiresNonNull.end());
  // RFC 0010: retains, decrements and known counts are may-facts.
  increments.insert(other.increments.begin(), other.increments.end());
  decrements.insert(other.decrements.begin(), other.decrements.end());
  counts.insert(other.counts.begin(), other.counts.end());
  // RFC 0011: a requirement of either side is a requirement.
  for (const auto &[param, requirements] : other.requiresExtent) {
    for (const ExtentRequirement &requirement : requirements)
      addRequirement(param, requirement);
  }
  // A path through either side that returns is a path that returns (RFC
  // 0009): the bit survives only when both sides have it.
  neverReturns =
      wasEmpty ? other.neverReturns : (neverReturns && other.neverReturns);
  if (wasEmpty) {
    outcomes = other.outcomes;
    nullOn = other.nullOn;
    nonNullOn = other.nonNullOn;
    storesOn = other.storesOn;
    factOn = other.factOn;
    return;
  }
  // Otherwise outcome knowledge is only as good as the least informed
  // side: a side that knows nothing about outcomes may return any class
  // with any of its effects, which the per-class maps cannot express.
  if (outcomes.empty() || other.outcomes.empty()) {
    outcomes.clear();
    nullOn.clear();
    nonNullOn.clear();
    storesOn.clear();
    factOn.clear();
    return;
  }
  // Per-class stores are may-facts per class (RFC 0010): a class either
  // side may return stores to what that side stores on it. Computed against
  // the classes before the merge, then normalised.
  {
    std::map<Outcome, std::set<SummaryPath>> joined = std::move(mineStoresOn);
    for (const auto &[outcome, perClass] : other.outcomes) {
      const std::set<SummaryPath> theirs = other.storesOnClass(outcome);
      joined[outcome].insert(theirs.begin(), theirs.end());
    }
    storesOn = std::move(joined);
  }
  // Per-class integer facts are must-facts (RFC 0010): a class both sides
  // may return keeps the paths both constrain, each fact joined; a class
  // only one side returns keeps that side's.
  {
    std::map<Outcome, OutcomeFacts> joined;
    for (const auto &[outcome, perClass] : other.outcomes) {
      const auto theirs = other.factOn.find(outcome);
      if (!outcomes.contains(outcome)) {
        if (theirs != other.factOn.end())
          joined[outcome] = theirs->second;
        continue;
      }
      const auto mine = factOn.find(outcome);
      if (mine == factOn.end() || theirs == other.factOn.end())
        continue;
      OutcomeFacts both;
      for (const auto &[path, fact] : mine->second) {
        const auto theirFact = theirs->second.find(path);
        if (theirFact == theirs->second.end())
          continue;
        ValueFact joinedFact = fact;
        joinedFact.join(theirFact->second);
        if (!joinedFact.trivial())
          both.emplace(path, joinedFact);
      }
      if (!both.empty())
        joined[outcome] = std::move(both);
    }
    for (const auto &[outcome, facts] : factOn) {
      if (!other.outcomes.contains(outcome))
        joined[outcome] = facts;
    }
    factOn = std::move(joined);
  }
  // Null and non-null facts are must-facts: a class both sides may return
  // keeps what both agree on; a class only one side returns keeps that
  // side's.
  const auto joinMust =
      [this, &other](const std::map<Outcome, std::set<SummaryPath>> &mine,
                     const std::map<Outcome, std::set<SummaryPath>> &theirs) {
        std::map<Outcome, std::set<SummaryPath>> joined;
        for (const auto &[outcome, perClass] : other.outcomes) {
          const auto theirPaths = theirs.find(outcome);
          if (!outcomes.contains(outcome)) {
            if (theirPaths != theirs.end())
              joined[outcome] = theirPaths->second;
            continue;
          }
          const auto minePaths = mine.find(outcome);
          if (minePaths == mine.end() || theirPaths == theirs.end())
            continue;
          std::set<SummaryPath> both;
          std::ranges::set_intersection(minePaths->second, theirPaths->second,
                                        std::inserter(both, both.end()));
          if (!both.empty())
            joined[outcome] = std::move(both);
        }
        for (const auto &[outcome, paths] : mine) {
          if (!other.outcomes.contains(outcome))
            joined[outcome] = paths;
        }
        return joined;
      };
  nullOn = joinMust(nullOn, other.nullOn);
  nonNullOn = joinMust(nonNullOn, other.nonNullOn);
  for (const auto &[outcome, theirs] : other.outcomes) {
    OutcomeEffects &mine = outcomes[outcome];
    for (const auto &[path, effect] : theirs)
      mine[path].join(effect);
  }
  normalizeStoresOn();
}

FunctionSummary remapGlobals(const FunctionSummary &summary,
                             const GlobalIdMap &map) {
  const auto remapPath =
      [&map](const SummaryPath &path) -> std::optional<SummaryPath> {
    if (!path.isGlobal())
      return path;
    const std::optional<std::uint32_t> id = map(path.index);
    if (!id)
      return std::nullopt;
    SummaryPath result = path;
    result.index = *id;
    return result;
  };
  // A conjunct on a dropped root is dropped: the guard weakens (RFC 0009).
  const auto remapGuard = [&remapPath](const PathGuard &guard) {
    PathGuard result;
    for (const auto &[path, fact] : guard.conditions) {
      if (const auto mapped = remapPath(path))
        result.conditions.emplace(*mapped, fact);
    }
    return result;
  };
  const auto remapEffect = [&remapGuard](const PlaceEffect &effect) {
    PlaceEffect result = effect;
    result.when = remapGuard(effect.when);
    return result;
  };
  // RFC 0011: an extent in a dropped global is unknown.
  const auto remapAffine =
      [&remapPath](const PathAffine &affine) -> std::optional<PathAffine> {
    if (!affine.path)
      return affine;
    const std::optional<SummaryPath> path = remapPath(*affine.path);
    if (!path)
      return std::nullopt;
    PathAffine result = affine;
    result.path = *path;
    return result;
  };
  const auto remapSource = [&remapPath, &remapGuard,
                            &remapAffine](const ValueSource &source) {
    ValueSource result = source;
    result.when = remapGuard(source.when);
    if (source.extent)
      result.extent = remapAffine(*source.extent);
    if ((source.kind != ValueSource::Kind::Copy &&
         source.kind != ValueSource::Kind::Borrow) ||
        !source.path)
      return result;
    const std::optional<SummaryPath> path = remapPath(*source.path);
    if (!path) {
      ValueSource unknown = ValueSource::unknown();
      unknown.when = result.when;
      return unknown;
    }
    result.path = *path;
    return result;
  };

  FunctionSummary result;
  for (const auto &[path, effect] : summary.effects) {
    if (const auto mapped = remapPath(path))
      result.addEffect(*mapped, remapEffect(effect));
  }
  for (const Store &store : summary.stores) {
    if (const auto dest = remapPath(store.dest))
      result.addStore(Store{.dest = *dest, .value = remapSource(store.value)});
  }
  for (const ValueSource &source : summary.returns)
    result.addReturn(remapSource(source));
  for (const auto &[outcome, effects] : summary.outcomes) {
    result.addOutcome(outcome);
    for (const auto &[path, effect] : effects) {
      if (const auto mapped = remapPath(path))
        result.addOutcome(outcome, *mapped, remapEffect(effect));
    }
  }
  for (const auto &[outcome, paths] : summary.nullOn) {
    for (const SummaryPath &path : paths) {
      if (const auto mapped = remapPath(path))
        result.nullOn[outcome].insert(*mapped);
    }
  }
  for (const auto &[outcome, paths] : summary.nonNullOn) {
    for (const SummaryPath &path : paths) {
      if (const auto mapped = remapPath(path))
        result.nonNullOn[outcome].insert(*mapped);
    }
  }
  const auto remapSet = [&remapPath](const std::set<SummaryPath> &paths) {
    std::set<SummaryPath> mappedPaths;
    for (const SummaryPath &path : paths) {
      if (const auto mapped = remapPath(path))
        mappedPaths.insert(*mapped);
    }
    return mappedPaths;
  };
  result.increments = remapSet(summary.increments);
  result.decrements = remapSet(summary.decrements);
  result.counts = remapSet(summary.counts);
  for (const auto &[outcome, paths] : summary.storesOn)
    result.storesOn[outcome] = remapSet(paths);
  for (const auto &[outcome, facts] : summary.factOn) {
    for (const auto &[path, fact] : facts) {
      if (const auto mapped = remapPath(path))
        result.factOn[outcome].emplace(*mapped, fact);
    }
  }
  result.requiresNonNull = summary.requiresNonNull;
  for (const auto &[param, requirements] : summary.requiresExtent) {
    for (const ExtentRequirement &requirement : requirements) {
      if (const auto need = remapAffine(requirement.need))
        result.addRequirement(
            param, ExtentRequirement{.need = *need,
                                     .when = remapGuard(requirement.when)});
    }
  }
  result.neverReturns = summary.neverReturns;
  result.normalizeStoresOn();
  return result;
}

std::string_view toString(ValueSource::Kind kind) noexcept {
  switch (kind) {
  case ValueSource::Kind::Fresh:
    return "fresh";
  case ValueSource::Kind::Copy:
    return "copy";
  case ValueSource::Kind::Borrow:
    return "borrow";
  case ValueSource::Kind::Null:
    return "null";
  case ValueSource::Kind::Unknown:
    return "unknown";
  case ValueSource::Kind::Raw:
    return "raw";
  }
  return "<invalid>";
}

} // namespace weavec::core
