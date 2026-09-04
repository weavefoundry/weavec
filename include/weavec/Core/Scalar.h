//===- Scalar.h - Value facts, guards and scalar tracking ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0009, *Value-conditional behaviour*.
//
// A `ValueFact` is what the checker knows about one value: the set of
// *classes* it may fall in (`zero`, `positive`, `negative` for an integer;
// `null`, `nonnull` for a pointer) and, when known, its exact constant. The
// classes are the RFC 0006 outcome classes, so a fact about a call result
// and a fact about an integer place have the same shape.
//
// A `GuardOn<Key>` is a conjunction of facts about keys (places in the
// state, summary paths in a summary) under which alone a record or an
// effect holds. A record takes as its guard the facts that held on the path
// that created it; at a merge, records present on both sides keep what
// their guards agree on. A guard is *refined* by a fact learnt later:
// refuted when the fact is disjoint from a conjunct, discharged when the
// fact implies it, narrowed otherwise.
//
// A `ScalarTracker` keeps the facts about integer places; pointer facts
// stay in `NullTracker` (RFC 0008).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_SCALAR_H
#define WEAVEC_CORE_SCALAR_H

#include "weavec/Core/Place.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

/// A class of value (RFC 0006, *Outcome-conditional summaries*; RFC 0009,
/// *Value facts*). Pointer values are `Null` or `NonNull`; integer values
/// are `Zero`, `Positive` or `Negative`.
enum class Outcome : std::uint8_t {
  Null,
  NonNull,
  Zero,
  Positive,
  Negative,
};

[[nodiscard]] std::string_view toString(Outcome outcome) noexcept;
[[nodiscard]] std::optional<Outcome>
parseOutcome(std::string_view text) noexcept;

/// A set of `Outcome`s as a bit mask. Facts are copied with every state and
/// every guard conjunct, so the set must not allocate. The interface is the
/// subset of `std::set<Outcome>` the checker uses; iteration is ascending.
class OutcomeSet {
public:
  // The iterator protocol's names (`iterator`, `value_type`, ...) are the
  // standard library's, not this project's.
  // NOLINTBEGIN(readability-identifier-naming)
  class iterator {
  public:
    using value_type = Outcome;
    using difference_type = std::ptrdiff_t;

    constexpr iterator() noexcept = default;
    constexpr iterator(std::uint8_t bits, std::uint8_t at) noexcept
        : bits(bits), at(at) {
      settle();
    }

    constexpr Outcome operator*() const noexcept {
      return static_cast<Outcome>(at);
    }
    constexpr iterator &operator++() noexcept {
      ++at;
      settle();
      return *this;
    }
    constexpr iterator operator++(int) noexcept {
      iterator copy = *this;
      ++*this;
      return copy;
    }
    friend constexpr bool operator==(const iterator &,
                                     const iterator &) noexcept = default;

  private:
    constexpr void settle() noexcept {
      while (at < Width && (bits & (1U << at)) == 0)
        ++at;
    }
    std::uint8_t bits = 0;
    std::uint8_t at = Width;
  };
  using const_iterator = iterator;
  using value_type = Outcome;
  // NOLINTEND(readability-identifier-naming)

  constexpr OutcomeSet() noexcept = default;
  constexpr OutcomeSet(std::initializer_list<Outcome> outcomes) noexcept {
    for (const Outcome outcome : outcomes)
      insert(outcome);
  }

  constexpr void insert(Outcome outcome) noexcept { bits |= bit(outcome); }
  template <typename Iterator>
  constexpr void insert(Iterator first, Iterator last) noexcept {
    for (; first != last; ++first)
      insert(*first);
  }
  constexpr void erase(Outcome outcome) noexcept {
    bits &= static_cast<std::uint8_t>(~bit(outcome));
  }
  [[nodiscard]] constexpr bool contains(Outcome outcome) const noexcept {
    return (bits & bit(outcome)) != 0;
  }
  [[nodiscard]] constexpr bool containsAll(OutcomeSet other) const noexcept {
    return (bits & other.bits) == other.bits;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits == 0; }
  [[nodiscard]] constexpr std::size_t size() const noexcept {
    std::size_t count = 0;
    for (std::uint8_t rest = bits; rest != 0;
         rest &= static_cast<std::uint8_t>(rest - 1))
      ++count;
    return count;
  }
  [[nodiscard]] constexpr iterator begin() const noexcept { return {bits, 0}; }
  [[nodiscard]] constexpr iterator end() const noexcept {
    return {bits, Width};
  }

  [[nodiscard]] constexpr OutcomeSet
  operator&(OutcomeSet other) const noexcept {
    OutcomeSet result;
    result.bits = bits & other.bits;
    return result;
  }
  [[nodiscard]] constexpr OutcomeSet
  operator|(OutcomeSet other) const noexcept {
    OutcomeSet result;
    result.bits = bits | other.bits;
    return result;
  }

  friend constexpr bool operator==(OutcomeSet, OutcomeSet) noexcept = default;
  friend constexpr std::strong_ordering
  operator<=>(OutcomeSet, OutcomeSet) noexcept = default;

private:
  static constexpr std::uint8_t Width = 5;
  static constexpr std::uint8_t bit(Outcome outcome) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(outcome));
  }
  std::uint8_t bits = 0;
};

/// What is known about one value: a non-empty set of classes and, for an
/// integer known exactly, its constant (whose class is then the only one).
struct ValueFact {
  OutcomeSet classes;
  std::optional<std::int64_t> constant;

  [[nodiscard]] static Outcome classOf(std::int64_t value) noexcept {
    if (value == 0)
      return Outcome::Zero;
    return value > 0 ? Outcome::Positive : Outcome::Negative;
  }
  [[nodiscard]] static ValueFact of(Outcome outcome) {
    return ValueFact{.classes = {outcome}, .constant = std::nullopt};
  }
  [[nodiscard]] static ValueFact of(std::initializer_list<Outcome> outcomes) {
    return ValueFact{.classes = OutcomeSet(outcomes), .constant = std::nullopt};
  }
  [[nodiscard]] static ValueFact ofConstant(std::int64_t value) {
    return ValueFact{.classes = {classOf(value)}, .constant = value};
  }
  /// `positive|negative`.
  [[nodiscard]] static ValueFact nonZero() {
    return of({Outcome::Positive, Outcome::Negative});
  }
  /// Every integer class: the trivial integer fact.
  [[nodiscard]] static ValueFact anyInteger() {
    return of({Outcome::Zero, Outcome::Positive, Outcome::Negative});
  }
  /// Both pointer classes: the trivial pointer fact.
  [[nodiscard]] static ValueFact anyPointer() {
    return of({Outcome::Null, Outcome::NonNull});
  }

  /// True if the fact excludes nothing: every integer class, or both
  /// pointer classes, without a constant.
  [[nodiscard]] bool trivial() const noexcept;
  /// True if the classes are pointer classes.
  [[nodiscard]] bool isPointer() const noexcept {
    return !classes.empty() && (classes.contains(Outcome::Null) ||
                                classes.contains(Outcome::NonNull));
  }
  [[nodiscard]] bool disjointFrom(const ValueFact &other) const noexcept;
  /// True if every value satisfying `this` satisfies `other`.
  [[nodiscard]] bool implies(const ValueFact &other) const noexcept;

  /// Union of the classes; the constant survives only when both agree.
  void join(const ValueFact &other);
  /// Intersection of the classes; the constant of whichever side has one.
  /// Returns false, leaving `this` unchanged, when the intersection is
  /// empty (the fact is refuted).
  [[nodiscard]] bool narrow(const ValueFact &other);

  /// `=3`, `zero`, `positive|negative`, `nonnull`.
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<ValueFact> parse(std::string_view text);

  friend bool operator==(const ValueFact &, const ValueFact &) = default;
  friend std::strong_ordering operator<=>(const ValueFact &,
                                          const ValueFact &) = default;
};

/// What refining a guard by a new fact did to it.
enum class GuardRefinement : std::uint8_t {
  /// No conjunct on the key.
  Unchanged,
  /// The conjunct on the key was narrowed.
  Narrowed,
  /// The fact implied the conjunct, which is now known to hold and dropped.
  Discharged,
  /// The fact is disjoint from the conjunct: whatever the guard protected
  /// does not hold here.
  Refuted,
};

/// The most conjuncts a guard carries: a guard is a bounded summary of the
/// path's facts, and dropping a conjunct only weakens it.
inline constexpr std::size_t MaxGuardConjuncts = 8;

/// A small map kept as a sorted vector: one allocation per guard rather than
/// one per conjunct, which matters because every record in every state
/// carries one and states are copied at every block. The interface is the
/// subset of `std::map` the guards use.
template <typename Key, typename Value>
class FlatMap {
public:
  // The container protocol's names are the standard library's.
  // NOLINTBEGIN(readability-identifier-naming)
  using value_type = std::pair<Key, Value>;
  using iterator = std::vector<value_type>::iterator;
  using const_iterator = std::vector<value_type>::const_iterator;
  // NOLINTEND(readability-identifier-naming)

  [[nodiscard]] iterator begin() noexcept { return entries.begin(); }
  [[nodiscard]] iterator end() noexcept { return entries.end(); }
  [[nodiscard]] const_iterator begin() const noexcept {
    return entries.begin();
  }
  [[nodiscard]] const_iterator end() const noexcept { return entries.end(); }
  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
  void clear() noexcept { entries.clear(); }

  [[nodiscard]] iterator find(const Key &key) {
    const auto it = lowerBound(key);
    return it != entries.end() && it->first == key ? it : entries.end();
  }
  [[nodiscard]] const_iterator find(const Key &key) const {
    const auto it = lowerBound(key);
    return it != entries.end() && it->first == key ? it : entries.end();
  }
  [[nodiscard]] bool contains(const Key &key) const {
    return find(key) != entries.end();
  }
  [[nodiscard]] const Value &at(const Key &key) const {
    return find(key)->second;
  }

  std::pair<iterator, bool> emplace(const Key &key, const Value &value) {
    const auto it = lowerBound(key);
    if (it != entries.end() && it->first == key)
      return {it, false};
    return {entries.emplace(it, key, value), true};
  }
  iterator erase(const_iterator it) { return entries.erase(it); }
  std::size_t erase(const Key &key) {
    const auto it = find(key);
    if (it == entries.end())
      return 0;
    entries.erase(it);
    return 1;
  }

  friend bool operator==(const FlatMap &, const FlatMap &) = default;
  friend auto operator<=>(const FlatMap &, const FlatMap &) = default;

private:
  [[nodiscard]] iterator lowerBound(const Key &key) {
    return std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const value_type &entry, const Key &k) { return entry.first < k; });
  }
  [[nodiscard]] const_iterator lowerBound(const Key &key) const {
    return std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const value_type &entry, const Key &k) { return entry.first < k; });
  }

  std::vector<value_type> entries;
};

/// A conjunction of facts about keys; empty means "always".
template <typename Key>
struct GuardOn {
  FlatMap<Key, ValueFact> conditions;

  [[nodiscard]] bool trivial() const noexcept { return conditions.empty(); }

  /// Conjoins "`key` satisfies `fact`". An existing conjunct on the key is
  /// narrowed; a contradiction (the guard already excludes every value of
  /// `fact`) cannot be expressed and drops the conjunct, which weakens the
  /// guard (the sound direction). A guard that is full ignores a new key.
  /// Returns whether the guard changed.
  bool require(const Key &key, const ValueFact &fact) {
    if (fact.trivial())
      return false;
    const auto it = conditions.find(key);
    if (it == conditions.end()) {
      if (conditions.size() >= MaxGuardConjuncts)
        return false;
      conditions.emplace(key, fact);
      return true;
    }
    if (it->second.implies(fact))
      return false;
    if (!it->second.narrow(fact))
      conditions.erase(it);
    return true;
  }

  /// A fact learnt on the current path: `Refuted` if it is disjoint from the
  /// conjunct on `key` (what the guard protects does not hold on this
  /// path); otherwise the conjunct is narrowed to it, or added (the guard
  /// is the path's facts, whichever came first).
  [[nodiscard]] GuardRefinement learn(const Key &key, const ValueFact &fact) {
    if (fact.trivial())
      return GuardRefinement::Unchanged;
    const auto it = conditions.find(key);
    if (it != conditions.end() && fact.disjointFrom(it->second))
      return GuardRefinement::Refuted;
    return require(key, fact) ? GuardRefinement::Narrowed
                              : GuardRefinement::Unchanged;
  }

  /// The guard holding on either of two merged paths: conjuncts on keys
  /// both sides constrain, each joined; a joined fact that excludes nothing
  /// is dropped. Returns whether `this` changed.
  bool join(const GuardOn &other) {
    bool changed = false;
    for (auto it = conditions.begin(); it != conditions.end();) {
      const auto theirs = other.conditions.find(it->first);
      if (theirs == other.conditions.end()) {
        it = conditions.erase(it);
        changed = true;
        continue;
      }
      const ValueFact before = it->second;
      it->second.join(theirs->second);
      if (it->second.trivial()) {
        it = conditions.erase(it);
        changed = true;
        continue;
      }
      changed |= it->second != before;
      ++it;
    }
    return changed;
  }

  [[nodiscard]] GuardRefinement refine(const Key &key, const ValueFact &fact) {
    const auto it = conditions.find(key);
    if (it == conditions.end())
      return GuardRefinement::Unchanged;
    if (fact.disjointFrom(it->second))
      return GuardRefinement::Refuted;
    if (fact.implies(it->second)) {
      conditions.erase(it);
      return GuardRefinement::Discharged;
    }
    const bool narrowed = it->second.narrow(fact);
    (void)narrowed; // not disjoint, so the intersection is non-empty
    return GuardRefinement::Narrowed;
  }

  /// Drops the conjunct on `key` (the key's value is no longer the one the
  /// guard spoke about). Returns whether there was one.
  bool drop(const Key &key) { return conditions.erase(key) > 0; }

  friend bool operator==(const GuardOn &, const GuardOn &) = default;
  friend std::strong_ordering operator<=>(const GuardOn &,
                                          const GuardOn &) = default;
};

/// A guard over places, carried by records in the dataflow state.
using PlaceGuard = GuardOn<PlaceId>;

/// The facts about integer places on the current path (RFC 0009, *Scalar
/// facts in the state*). A place with no entry may hold any value.
class ScalarTracker {
public:
  /// `place` now satisfies `fact`, whatever was known before. A trivial fact
  /// erases the entry.
  void set(PlaceId place, ValueFact fact);

  /// Narrows what is known about `place` by `fact` (a condition edge).
  /// Returns `Refuted`, changing nothing, if the edge is infeasible as far
  /// as the facts know.
  GuardRefinement narrow(PlaceId place, const ValueFact &fact);

  [[nodiscard]] std::optional<ValueFact> factOf(PlaceId place) const;

  /// Forgets what is known about `place` (reassigned, dead).
  void forget(PlaceId place);

  /// Per place, the join of the two facts; a place with a fact on one side
  /// only has none after. Returns whether this tracker changed.
  bool join(const ScalarTracker &other);

  /// Places with a fact, ascending (for dumps).
  [[nodiscard]] std::vector<PlaceId> places() const;
  [[nodiscard]] const std::map<PlaceId, ValueFact> &all() const noexcept {
    return facts;
  }

  [[nodiscard]] bool empty() const noexcept { return facts.empty(); }

  friend bool operator==(const ScalarTracker &,
                         const ScalarTracker &) = default;

private:
  std::map<PlaceId, ValueFact> facts;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_SCALAR_H
