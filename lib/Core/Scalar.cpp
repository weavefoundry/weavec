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

ValueFact ValueFact::ofInteger(const IntegerRange &range) {
  ValueFact result;
  if (range.empty())
    return result;
  if (const auto value = range.constant()) {
    if (const auto exact = value->signedValue())
      return ofConstant(*exact);
  }
  // A type's full domain is not an additional path condition. The AST or
  // expression leaf supplies that type whenever the fact is read again.
  if (range.isFull())
    return anyInteger();
  if (!range.type.isSigned && range.minimum()->bits == 1 &&
      range.maximum()->bits == range.type.mask() && range.all().size() == 1)
    return nonZero();
  const auto zero = IntegerValue::ofBits(range.type, 0);
  if (range.contains(zero))
    result.classes.insert(Outcome::Zero);
  if (range.minimum()->negative())
    result.classes.insert(Outcome::Negative);
  if (!range.maximum()->negative() && range.maximum()->bits != 0)
    result.classes.insert(Outcome::Positive);
  if (result.inType(range.type) != range)
    result.integer = range;
  return result;
}

IntegerRange ValueFact::inType(IntegerType type) const {
  if (integer)
    return integer->converted(type);
  if (constant)
    return IntegerRange::singleton(
        IntegerValue::ofBits(type, static_cast<std::uint64_t>(*constant)));
  std::vector<IntegerInterval> ranges;
  const auto zero = type.rank(0);
  if (classes.contains(Outcome::Negative) && type.isSigned)
    ranges.push_back({.lower = 0, .upper = zero - 1});
  if (classes.contains(Outcome::Zero))
    ranges.push_back({.lower = zero, .upper = zero});
  if (classes.contains(Outcome::Positive) && zero < type.mask())
    ranges.push_back({.lower = zero + 1, .upper = type.mask()});
  return IntegerRange::fromRanks(type, std::move(ranges));
}

bool ValueFact::trivial() const noexcept {
  if (constant || integer)
    return false;
  if (isPointer())
    return classes.contains(Outcome::Null) &&
           classes.contains(Outcome::NonNull);
  return classes.contains(Outcome::Zero) &&
         classes.contains(Outcome::Positive) &&
         classes.contains(Outcome::Negative);
}

bool ValueFact::disjointFrom(const ValueFact &other) const {
  if (integer || other.integer) {
    const auto type = integer ? integer->type : other.integer->type;
    if (inType(type).disjoint(other.inType(type)))
      return true;
  }
  if (constant && other.constant)
    return *constant != *other.constant;
  return (classes & other.classes).empty();
}

bool ValueFact::implies(const ValueFact &other) const {
  if (other.integer && !other.integer->contains(inType(other.integer->type)))
    return false;
  if (other.constant)
    return constant == other.constant;
  return other.classes.containsAll(classes);
}

void ValueFact::join(const ValueFact &other) {
  if (integer || other.integer) {
    const auto type = integer ? integer->type : other.integer->type;
    const auto joined = inType(type).united(other.inType(type));
    *this = ofInteger(joined);
    return;
  }
  classes = classes | other.classes;
  if (constant != other.constant)
    constant.reset();
}

bool ValueFact::narrow(const ValueFact &other) {
  if (disjointFrom(other))
    return false;
  if (integer || other.integer) {
    const auto type = integer ? integer->type : other.integer->type;
    *this = ofInteger(inType(type).intersect(other.inType(type)));
    return true;
  }
  classes = classes & other.classes;
  if (!constant)
    constant = other.constant;
  return true;
}

std::string ValueFact::toString() const {
  if (integer)
    return "range(" + integer->toString() + ")";
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
  if (text.starts_with("range(") && text.ends_with(')')) {
    const auto range = IntegerRange::parse(text.substr(6, text.size() - 7));
    if (!range || range->empty())
      return std::nullopt;
    return ofInteger(*range);
  }
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

bool ScalarTracker::join(const ScalarTracker &other, bool widenRanges) {
  bool changed = false;
  for (auto it = facts.begin(); it != facts.end();) {
    const auto theirs = other.facts.find(it->first);
    if (theirs == other.facts.end()) {
      it = facts.erase(it);
      changed = true;
      continue;
    }
    const ValueFact before = it->second;
    if (it->second.integer || theirs->second.integer ||
        (!widenRanges && it->second.constant && theirs->second.constant)) {
      auto type = IntegerType{.width = 64, .isSigned = true};
      if (it->second.integer)
        type = it->second.integer->type;
      else if (theirs->second.integer)
        type = theirs->second.integer->type;
      const auto a = it->second.inType(type);
      const auto b = theirs->second.inType(type);
      it->second =
          ValueFact::ofInteger(widenRanges ? a.widened(b) : a.united(b));
    } else {
      it->second.join(theirs->second);
    }
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
