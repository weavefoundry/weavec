//===- Summary.cpp - Function summaries for signature inference -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Summary.h"

#include <algorithm>
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

void PlaceEffect::join(const PlaceEffect &other) noexcept {
  read = read || other.read;
  written = written || other.written;
  freed = freed || other.freed;
  moved = moved || other.moved;
}

PlaceEffect FunctionSummary::effectOf(const SummaryPath &path) const {
  const auto it = effects.find(path);
  return it == effects.end() ? PlaceEffect{} : it->second;
}

void FunctionSummary::addEffect(const SummaryPath &path, PlaceEffect effect) {
  if (effect.empty())
    return;
  effects[path].join(effect);
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
  // shared use of it even when nothing was read through it.
  const auto usesPointee = [&](const ValueSource &value) {
    return (value.kind == ValueSource::Kind::Copy ||
            value.kind == ValueSource::Kind::Borrow) &&
           value.path &&
           (*value.path == pointee || pointee.isProperPrefixOf(*value.path));
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
  if (returns.contains(ValueSource::raw()))
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

void FunctionSummary::addOutcome(Outcome outcome, const SummaryPath &path,
                                 PlaceEffect effect) {
  OutcomeEffects &perClass = outcomes[outcome];
  if (!effect.empty())
    perClass[path].join(effect);
}

bool FunctionSummary::consumesUnconditionally(const SummaryPath &path) const {
  if (outcomes.empty())
    return true;
  return std::ranges::all_of(outcomes, [&path](const auto &entry) {
    const auto it = entry.second.find(path);
    return it != entry.second.end() && it->second.consumed();
  });
}

void FunctionSummary::join(const FunctionSummary &other) {
  // The empty summary is the bottom of the lattice (a join of candidates
  // starts from it): the other side's classes are the answer.
  const bool wasEmpty = empty();
  for (const auto &[path, effect] : other.effects)
    addEffect(path, effect);
  stores.insert(other.stores.begin(), other.stores.end());
  returns.insert(other.returns.begin(), other.returns.end());
  if (wasEmpty) {
    outcomes = other.outcomes;
    return;
  }
  // Otherwise outcome knowledge is only as good as the least informed
  // side: a side that knows nothing about outcomes may return any class
  // with any of its effects, which the per-class maps cannot express.
  if (outcomes.empty() || other.outcomes.empty()) {
    outcomes.clear();
    return;
  }
  for (const auto &[outcome, theirs] : other.outcomes) {
    OutcomeEffects &mine = outcomes[outcome];
    for (const auto &[path, effect] : theirs)
      mine[path].join(effect);
  }
}

FunctionSummary remapGlobals(const FunctionSummary &summary,
                             const GlobalIdMap &map) {
  const auto remapPath =
      [&map](const SummaryPath &path) -> std::optional<SummaryPath> {
    if (path.isParam())
      return path;
    const std::optional<std::uint32_t> id = map(path.index);
    if (!id)
      return std::nullopt;
    SummaryPath result = path;
    result.index = *id;
    return result;
  };
  const auto remapSource = [&remapPath](const ValueSource &source) {
    if ((source.kind != ValueSource::Kind::Copy &&
         source.kind != ValueSource::Kind::Borrow) ||
        !source.path)
      return source;
    const std::optional<SummaryPath> path = remapPath(*source.path);
    if (!path)
      return ValueSource::unknown();
    if (source.kind == ValueSource::Kind::Borrow)
      return ValueSource::borrow(*path);
    return source.interior ? ValueSource::interiorCopy(*path)
                           : ValueSource::copy(*path);
  };

  FunctionSummary result;
  for (const auto &[path, effect] : summary.effects) {
    if (const auto mapped = remapPath(path))
      result.addEffect(*mapped, effect);
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
        result.addOutcome(outcome, *mapped, effect);
    }
  }
  return result;
}

std::string_view toString(Outcome outcome) noexcept {
  switch (outcome) {
  case Outcome::Null:
    return "null";
  case Outcome::NonNull:
    return "nonnull";
  case Outcome::Zero:
    return "zero";
  case Outcome::Positive:
    return "positive";
  case Outcome::Negative:
    return "negative";
  }
  return "<invalid>";
}

std::optional<Outcome> parseOutcome(std::string_view text) noexcept {
  for (const Outcome outcome : {Outcome::Null, Outcome::NonNull, Outcome::Zero,
                                Outcome::Positive, Outcome::Negative}) {
    if (toString(outcome) == text)
      return outcome;
  }
  return std::nullopt;
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
