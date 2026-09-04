//===- Scalar.cpp - Value facts, guards and scalar tracking ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Scalar.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace weavec::core {

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

bool ValueFact::trivial() const noexcept {
  if (constant)
    return false;
  if (isPointer())
    return classes.contains(Outcome::Null) &&
           classes.contains(Outcome::NonNull);
  return classes.contains(Outcome::Zero) &&
         classes.contains(Outcome::Positive) &&
         classes.contains(Outcome::Negative);
}

bool ValueFact::disjointFrom(const ValueFact &other) const noexcept {
  if (constant && other.constant)
    return *constant != *other.constant;
  return (classes & other.classes).empty();
}

bool ValueFact::implies(const ValueFact &other) const noexcept {
  if (other.constant)
    return constant == other.constant;
  return other.classes.containsAll(classes);
}

void ValueFact::join(const ValueFact &other) {
  classes = classes | other.classes;
  if (constant != other.constant)
    constant.reset();
}

bool ValueFact::narrow(const ValueFact &other) {
  if (disjointFrom(other))
    return false;
  classes = classes & other.classes;
  if (!constant)
    constant = other.constant;
  return true;
}

std::string ValueFact::toString() const {
  if (constant)
    return "=" + std::to_string(*constant);
  std::string text;
  for (const Outcome outcome : classes) {
    if (!text.empty())
      text += '|';
    text += core::toString(outcome);
  }
  return text;
}

std::optional<ValueFact> ValueFact::parse(std::string_view text) {
  if (text.empty())
    return std::nullopt;
  if (text.front() == '=') {
    std::int64_t value = 0;
    const std::string_view digits = text.substr(1);
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size())
      return std::nullopt;
    return ofConstant(value);
  }
  ValueFact fact;
  while (!text.empty()) {
    const std::size_t bar = text.find('|');
    const std::string_view word =
        bar == std::string_view::npos ? text : text.substr(0, bar);
    const std::optional<Outcome> outcome = parseOutcome(word);
    if (!outcome)
      return std::nullopt;
    fact.classes.insert(*outcome);
    text = bar == std::string_view::npos ? std::string_view{}
                                         : text.substr(bar + 1);
  }
  if (fact.classes.empty())
    return std::nullopt;
  const bool pointer = fact.isPointer();
  if (std::ranges::any_of(fact.classes, [pointer](Outcome outcome) {
        const bool isPointerClass =
            outcome == Outcome::Null || outcome == Outcome::NonNull;
        return isPointerClass != pointer;
      }))
    return std::nullopt;
  return fact;
}

void ScalarTracker::set(PlaceId place, ValueFact fact) {
  if (fact.trivial()) {
    facts.erase(place);
    return;
  }
  facts.insert_or_assign(place, fact);
}

GuardRefinement ScalarTracker::narrow(PlaceId place, const ValueFact &fact) {
  if (fact.trivial())
    return GuardRefinement::Unchanged;
  auto [it, inserted] = facts.try_emplace(place, fact);
  if (inserted)
    return GuardRefinement::Narrowed;
  if (fact.disjointFrom(it->second))
    return GuardRefinement::Refuted;
  if (it->second.implies(fact))
    return GuardRefinement::Unchanged;
  (void)it->second.narrow(fact);
  return GuardRefinement::Narrowed;
}

std::optional<ValueFact> ScalarTracker::factOf(PlaceId place) const {
  const auto it = facts.find(place);
  if (it == facts.end())
    return std::nullopt;
  return it->second;
}

void ScalarTracker::forget(PlaceId place) {
  facts.erase(place);
}

bool ScalarTracker::join(const ScalarTracker &other) {
  bool changed = false;
  for (auto it = facts.begin(); it != facts.end();) {
    const auto theirs = other.facts.find(it->first);
    if (theirs == other.facts.end()) {
      it = facts.erase(it);
      changed = true;
      continue;
    }
    const ValueFact before = it->second;
    it->second.join(theirs->second);
    if (it->second.trivial()) {
      it = facts.erase(it);
      changed = true;
      continue;
    }
    changed |= it->second != before;
    ++it;
  }
  return changed;
}

std::vector<PlaceId> ScalarTracker::places() const {
  std::vector<PlaceId> result;
  result.reserve(facts.size());
  for (const auto &[place, fact] : facts)
    result.push_back(place);
  return result;
}

} // namespace weavec::core
