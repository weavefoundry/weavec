//===- Safety.cpp - Checked obligations and positive evidence ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Safety.h"

#include "weavec/Core/SafetyEntryPool.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <tuple>

namespace weavec::core {

struct SafetyLedger::Storage {
  SafetyEntries entries;
  mutable std::shared_ptr<const SafetyPropagation> propagation;
  // A weak pool does not increase use_count, but its indexed snapshot must
  // remain immutable even after every other strong owner goes away.
  bool published = false;
};

void SafetyLedger::shareSnapshot() {
  if (!obligations || obligations->published)
    return;
  obligations =
      SafetyEntryPool::internSnapshot(obligations, obligations->entries);
  obligations->published = true;
}

void SafetyEntries::set(std::string key, SafetyObligation obligation) {
  set(SafetyEntryPool::intern(std::move(key), std::move(obligation)));
}

void SafetyEntries::set(std::shared_ptr<const Row> row) {
  const auto position = index.lower_bound(row->first);
  set(std::move(row), position);
}

void SafetyEntries::set(std::shared_ptr<const Row> row,
                        Index::const_iterator position) {
  // Erase before the old Row can die: retaining its string_view as the key
  // after replacing the owner would leave a dangling view.
  if (position == index.end() || row->first < position->first) {
    index.emplace_hint(position, std::string_view(row->first), row);
  } else {
    const auto next = std::next(position);
    auto node = index.extract(position);
    node.key() = row->first;
    node.mapped() = std::move(row);
    index.insert(next, std::move(node));
  }
}

bool operator==(const SafetyEntries &left, const SafetyEntries &right) {
  return std::ranges::equal(
      left.index, right.index, [](const auto &a, const auto &b) {
        return a.second == b.second || *a.second == *b.second;
      });
}

const SafetyEntries &SafetyLedger::entries() const {
  static const SafetyEntries Empty;
  return obligations ? obligations->entries : Empty;
}

static constexpr std::array<std::string_view, 5> Outcomes{
    "proven", "required", "trusted", "unresolved", "violation"};
static constexpr std::array<std::string_view, 9> Properties{
    "bounds", "validity",  "initialization", "release", "aliasing",
    "call",   "semantics", "arithmetic",     "resource"};

std::string_view toString(SafetyOutcome value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  return index < Outcomes.size() ? std::span(Outcomes)[index] : "unresolved";
}
std::string_view toString(SafetyProperty value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  return index < Properties.size() ? std::span(Properties)[index] : "semantics";
}
std::optional<SafetyOutcome> parseSafetyOutcome(std::string_view value) {
  for (std::size_t i = 0; i < Outcomes.size(); ++i)
    if (Outcomes.at(i) == value)
      return static_cast<SafetyOutcome>(i);
  return std::nullopt;
}
std::optional<SafetyProperty> parseSafetyProperty(std::string_view value) {
  for (std::size_t i = 0; i < Properties.size(); ++i)
    if (Properties.at(i) == value)
      return static_cast<SafetyProperty>(i);
  return std::nullopt;
}

static std::string identityPrefix(std::string_view file, std::uint32_t line,
                                  std::uint32_t column,
                                  std::string_view function,
                                  SafetyProperty property) {
  // Length-delimited JSON strings cannot collide on embedded separators.
  auto result = safetyJsonString(file);
  result.reserve(result.size() + function.size() + 64);
  result += ':';
  result += std::to_string(line);
  result += ':';
  result += std::to_string(column);
  result += ':';
  result += safetyJsonString(function);
  result += ':';
  result += toString(property);
  result += ':';
  return result;
}

std::string SafetyObligation::identity() const {
  return identityPrefix(location.file, location.line, location.column, function,
                        property) +
         safetyJsonString(subject);
}

static bool preferSafetyCalls(const SafetyCallPath &candidate,
                              const SafetyCallPath &current) {
  if (candidate.size() != current.size())
    return candidate.size() < current.size();
  if (candidate == current)
    return false;
  const auto provenance = [](const auto &calls) {
    std::string result;
    for (const auto &call : calls)
      result += safetyJsonString(call.file) + ":" + std::to_string(call.line) +
                ":" + std::to_string(call.column) + ";";
    return result;
  };
  return provenance(candidate) < provenance(current);
}

const SafetyPropagation &SafetyLedger::propagation() const {
  static const SafetyPropagation Empty;
  if (!obligations)
    return Empty;
  if (obligations->propagation)
    return *obligations->propagation;
  auto result = std::make_shared<SafetyPropagation>();
  using Origin = std::tuple<std::string_view, std::uint32_t, std::uint32_t,
                            std::string_view>;
  const auto collect = [&](bool trusted, auto &output) {
    std::map<Origin, std::size_t> positions;
    for (const auto &[key, entry] : entries()) {
      (void)key;
      if (trusted ? entry.outcome != SafetyOutcome::Trusted
                  : entry.outcome < SafetyOutcome::Unresolved)
        continue;
      const auto &origin =
          entry.calls.empty() ? entry.location : entry.calls.back();
      const Origin identity{origin.file, origin.line, origin.column,
                            entry.reason};
      const auto [position, inserted] =
          positions.try_emplace(identity, output.size());
      auto calls = entry.calls;
      calls.insert(calls.begin(), entry.location);
      calls.normalize();
      if (inserted) {
        output.push_back(
            SafetyObligation{.property = SafetyProperty::Call,
                             .outcome = trusted ? SafetyOutcome::Trusted
                                                : SafetyOutcome::Unresolved,
                             .location = origin,
                             .function = {},
                             .subject = {},
                             .reason = entry.reason,
                             .calls = std::move(calls)});
      } else if (preferSafetyCalls(calls, output[position->second].calls)) {
        output[position->second].calls = std::move(calls);
      }
    }
  };
  // Preserve first-seen order separately for the two original propagation
  // passes. The caller's ordinary ledger join still handles unsafe conversion
  // and collisions between trust and unresolved evidence at its own cap.
  collect(false, result->unresolved);
  collect(true, result->trusted);
  obligations->propagation = std::move(result);
  return *obligations->propagation;
}

void SafetyLedger::add(SafetyObligation obligation) {
  obligation.location.opaque = 0;
  const auto bound = [&](std::string &value) {
    if (value.size() > 65536) {
      value.resize(65536);
      exhausted = true;
    }
  };
  bound(obligation.location.file);
  bound(obligation.function);
  bound(obligation.subject);
  bound(obligation.reason);
  auto key = obligation.identity();
  const auto found = entries().find(key);
  if (rejects(found, obligation.outcome, obligation.reason))
    return;
  addPrepared(std::move(key), std::move(obligation), found);
}

void SafetyLedger::addCalls(std::span<const SafetyObligation> origins,
                            const SourceLocation &location,
                            std::string_view function, std::string_view callee,
                            bool unsafe) {
  if (origins.empty())
    return;
  // Inserting into this ledger can invalidate its own cached projection.
  // Keep that backing vector alive if the caller borrowed our origins.
  const auto projectionOwner = obligations ? obligations->propagation : nullptr;
  const auto bound = [&](std::string_view value) {
    if (value.size() > 65536) {
      exhausted = true;
      value = value.substr(0, 65536);
    }
    return value;
  };
  const SourceLocation callsite{.file = std::string(bound(location.file)),
                                .line = location.line,
                                .column = location.column,
                                .opaque = 0};
  const std::string caller(bound(function));
  const std::string called(callee);
  const auto prefix =
      identityPrefix(callsite.file, callsite.line, callsite.column, caller,
                     SafetyProperty::Call);
  for (const auto &entry : origins) {
    const auto &origin =
        entry.calls.empty() ? entry.location : entry.calls.back();
    const auto subject = identityPrefix(origin.file, origin.line, origin.column,
                                        called, SafetyProperty::Call) +
                         safetyJsonString(entry.reason);
    const auto boundedSubject = bound(subject);
    std::string unsafeReason;
    if (unsafe)
      unsafeReason = "unsafe boundary: " + entry.reason;
    const auto reason = bound(unsafe ? std::string_view(unsafeReason)
                                     : std::string_view(entry.reason));
    const auto outcome = unsafe ? SafetyOutcome::Trusted : entry.outcome;
    auto key = prefix + safetyJsonString(boundedSubject);
    const auto found = entries().find(key);
    if (rejects(found, outcome, reason))
      continue;
    auto calls = entry.calls;
    calls.normalize();
    if (found != entries().end() && found->second.outcome == outcome &&
        found->second.reason == reason &&
        !preferSafetyCalls(calls, found->second.calls))
      continue;
    addPrepared(std::move(key),
                {.property = SafetyProperty::Call,
                 .outcome = outcome,
                 .location = callsite,
                 .function = caller,
                 .subject = std::string(boundedSubject),
                 .reason = std::string(reason),
                 .calls = std::move(calls)},
                found);
  }
}

static std::shared_ptr<const PreparedSafetyOrigins>
prepareCallOrigins(std::span<const SafetyObligation> origins,
                   std::string_view callee, bool unsafe) {
  auto result = std::make_shared<PreparedSafetyOrigins>();
  result->entries.reserve(origins.size());
  for (const auto &entry : origins) {
    const auto &origin =
        entry.calls.empty() ? entry.location : entry.calls.back();
    PreparedSafetyOrigins::Entry prepared;
    prepared.subject = identityPrefix(origin.file, origin.line, origin.column,
                                      callee, SafetyProperty::Call) +
                       safetyJsonString(entry.reason);
    prepared.reason =
        unsafe ? "unsafe boundary: " + entry.reason : entry.reason;
    for (auto *text : {&prepared.subject, &prepared.reason}) {
      if (text->size() > 65536) {
        text->resize(65536);
        prepared.limited = true;
      }
    }
    prepared.escapedSubject = safetyJsonString(prepared.subject);
    result->bytes += prepared.subject.capacity() +
                     prepared.escapedSubject.capacity() +
                     prepared.reason.capacity() + 3;
    result->entries.push_back(std::move(prepared));
  }
  result->bytes +=
      sizeof(PreparedSafetyOrigins) +
      (result->entries.capacity() * sizeof(PreparedSafetyOrigins::Entry));
  return result;
}

void SafetyLedger::addCalls(const SafetyLedger &source, bool trusted,
                            const SourceLocation &location,
                            std::string_view function, std::string_view callee,
                            bool unsafe) {
  const auto &projection = source.propagation();
  const auto &origins = trusted ? projection.trusted : projection.unresolved;
  if (origins.empty())
    return;
  if (!SafetyEntryPool::cachesCalls() || callee.size() > 4096) {
    addCalls(origins, location, function, callee, unsafe);
    return;
  }
  // Keep the exact projection alive even when source is this ledger and its
  // first insertion invalidates the cached projection in Storage.
  const auto owner = source.obligations->propagation;
  SafetyEntryPool::CallKey key{.source = owner.get(),
                               .callee = std::string(callee),
                               .trusted = trusted,
                               .unsafe = unsafe};
  auto prepared = SafetyEntryPool::findCalls(key, owner);
  if (!prepared) {
    prepared = prepareCallOrigins(origins, callee, unsafe);
    SafetyEntryPool::saveCalls(std::move(key), owner, prepared);
  }
  const auto bound = [&](std::string_view value) {
    if (value.size() > 65536) {
      exhausted = true;
      value = value.substr(0, 65536);
    }
    return value;
  };
  const SourceLocation callsite{.file = std::string(bound(location.file)),
                                .line = location.line,
                                .column = location.column,
                                .opaque = 0};
  const std::string caller(bound(function));
  const auto prefix =
      identityPrefix(callsite.file, callsite.line, callsite.column, caller,
                     SafetyProperty::Call);
  for (std::size_t i = 0; i < origins.size(); ++i) {
    const auto &entry = origins.at(i);
    const auto &strings = prepared->entries.at(i);
    exhausted |= strings.limited;
    const auto outcome = unsafe ? SafetyOutcome::Trusted : entry.outcome;
    auto identity = prefix + strings.escapedSubject;
    const auto found = entries().find(identity);
    if (rejects(found, outcome, strings.reason))
      continue;
    auto calls = entry.calls;
    calls.normalize();
    if (found != entries().end() && found->second.outcome == outcome &&
        found->second.reason == strings.reason &&
        !preferSafetyCalls(calls, found->second.calls))
      continue;
    addPrepared(std::move(identity),
                {.property = SafetyProperty::Call,
                 .outcome = outcome,
                 .location = callsite,
                 .function = caller,
                 .subject = strings.subject,
                 .reason = strings.reason,
                 .calls = std::move(calls)},
                found);
  }
}

bool SafetyLedger::rejects(SafetyEntries::ConstIterator found,
                           SafetyOutcome outcome, std::string_view reason) {
  // Provenance cannot change a rejected key or a strictly weaker candidate.
  // Large incomplete callees repeatedly propagate both; avoid normalizing
  // their discarded paths while retaining the original exhaustion behavior.
  if (found == entries().end()) {
    if (entries().size() == MaxSafetyObligations) {
      exhausted = true;
      return true;
    }
  } else if (outcome < found->second.outcome ||
             (outcome == found->second.outcome &&
              reason > found->second.reason)) {
    return true;
  }
  return false;
}

void SafetyLedger::addPrepared(std::string key, SafetyObligation obligation,
                               SafetyEntries::ConstIterator found) {
  obligation.calls.normalize();
  if (found != entries().end() && found->second == obligation)
    return;
  const auto writable = [&]() -> auto & {
    if (!obligations)
      obligations = std::make_shared<Storage>();
    else if (obligations.use_count() != 1 || obligations->published)
      obligations = std::make_shared<Storage>(*obligations);
    obligations->published = false;
    obligations->propagation.reset();
    return obligations->entries;
  };
  if (found == entries().end()) {
    weakest = std::max(weakest, obligation.outcome);
    trustedEntries += obligation.outcome == SafetyOutcome::Trusted ? 1U : 0U;
    writable().set(std::move(key), std::move(obligation));
  } else if (obligation.outcome > found->second.outcome ||
             (obligation.outcome == found->second.outcome &&
              (obligation.reason < found->second.reason ||
               (obligation.reason == found->second.reason &&
                preferSafetyCalls(obligation.calls, found->second.calls))))) {
    trustedEntries -= found->second.outcome == SafetyOutcome::Trusted ? 1U : 0U;
    trustedEntries += obligation.outcome == SafetyOutcome::Trusted ? 1U : 0U;
    weakest = std::max(weakest, obligation.outcome);
    writable().set(std::move(key), std::move(obligation));
  }
}

void SafetyLedger::join(const SafetyLedger &other) {
  if (this == &other)
    return;
  exhausted |= other.exhausted;
  if (!obligations) {
    obligations = other.obligations;
    weakest = other.weakest;
    trustedEntries = other.trustedEntries;
    return;
  }
  if (!other.obligations || obligations == other.obligations)
    return;
  auto position = obligations->entries.index.cbegin();
  for (const auto &[key, row] : other.obligations->entries.index) {
    while (position != obligations->entries.index.cend() &&
           position->first < key)
      ++position;
    const bool presentKey =
        position != obligations->entries.index.cend() && position->first == key;
    if (presentKey && position->second == row)
      continue;
    const auto &obligation = row->second;
    const auto found =
        presentKey ? SafetyEntries::ConstIterator(position) : entries().end();
    if (rejects(found, obligation.outcome, obligation.reason))
      continue;
    if (found != entries().end()) {
      const auto &present = found->second;
      if (present.outcome == obligation.outcome &&
          present.reason == obligation.reason &&
          !preferSafetyCalls(obligation.calls, present.calls))
        continue;
      trustedEntries -= present.outcome == SafetyOutcome::Trusted ? 1U : 0U;
    }
    trustedEntries += obligation.outcome == SafetyOutcome::Trusted ? 1U : 0U;
    weakest = std::max(weakest, obligation.outcome);
    if (obligations.use_count() != 1 || obligations->published) {
      obligations = std::make_shared<Storage>(*obligations);
      // A published snapshot can have only one strong owner. Its old
      // iterators may already be dead after detachment, so reacquire here.
      position = obligations->entries.index.lower_bound(key);
    }
    obligations->published = false;
    obligations->propagation.reset();
    const auto next = presentKey ? std::next(position) : position;
    obligations->entries.set(row, position);
    position = next;
  }
}
bool SafetyLedger::complete() const {
  return !exhausted && weakest < SafetyOutcome::Unresolved;
}
bool SafetyLedger::violated() const {
  return weakest == SafetyOutcome::Violation;
}
bool SafetyLedger::trusted() const {
  return trustedEntries != 0;
}

bool operator==(const SafetyLedger &left, const SafetyLedger &right) {
  if (left.exhausted != right.exhausted)
    return false;
  if (left.obligations == right.obligations)
    return true;
  if (left.entries().size() != right.entries().size())
    return false;
  return std::ranges::equal(
      left.entries(), right.entries(), [](const auto &a, const auto &b) {
        return a.first == b.first && a.second.outcome == b.second.outcome;
      });
}

static std::optional<ValueFact> exactConditionUnion(const ValueFact &a,
                                                    const ValueFact &b) {
  if (a == b)
    return a;
  if (a.isPointer() && b.isPointer()) {
    auto joined = a;
    joined.join(b);
    return joined;
  }
  if (!a.integer || !b.integer || a.integer->type != b.integer->type)
    return std::nullopt;
  auto intervals = a.integer->all();
  intervals.insert(intervals.end(), b.integer->all().begin(),
                   b.integer->all().end());
  std::ranges::sort(intervals);
  std::vector<IntegerInterval> exact;
  for (const auto &interval : intervals) {
    if (!exact.empty() && (interval.lower <= exact.back().upper ||
                           (exact.back().upper != UINT64_MAX &&
                            interval.lower == exact.back().upper + 1)))
      exact.back().upper = std::max(exact.back().upper, interval.upper);
    else
      exact.push_back(interval);
  }
  const auto joined = a.integer->united(*b.integer);
  // A range-domain cap may fill holes. That is safe for possible values,
  // but cannot widen the condition under which a must-fact is claimed.
  return joined.all() == exact ? std::optional(ValueFact::ofInteger(joined))
                               : std::nullopt;
}

static bool mergeInitializedConditions(std::vector<InitializedRange> &ranges) {
  bool changed = false;
  for (std::size_t i = 0; i < ranges.size(); ++i)
    for (std::size_t j = i + 1; j < ranges.size();) {
      auto &a = ranges[i];
      const auto &b = ranges[j];
      if (a.begin != b.begin || a.end != b.end || a.source != b.source ||
          a.zeroed != b.zeroed || a.when.pointers != b.when.pointers ||
          a.when.integers != b.when.integers) {
        ++j;
        continue;
      }
      std::set<PlaceId> keys;
      for (const auto &[key, fact] : a.when.conditions) {
        (void)fact;
        keys.insert(key);
      }
      for (const auto &[key, fact] : b.when.conditions) {
        (void)fact;
        keys.insert(key);
      }
      std::optional<PlaceId> different;
      bool compatible = true;
      for (const auto key : keys) {
        const auto x = a.when.conditions.find(key);
        const auto y = b.when.conditions.find(key);
        if (x != a.when.conditions.end() && y != b.when.conditions.end() &&
            x->second == y->second)
          continue;
        if (different) {
          compatible = false;
          break;
        }
        different = key;
      }
      if (compatible && different) {
        const auto x = a.when.conditions.find(*different);
        const auto y = b.when.conditions.find(*different);
        if (x == a.when.conditions.end() || y == b.when.conditions.end()) {
          a.when.conditions.erase(*different);
        } else if (const auto joined =
                       exactConditionUnion(x->second, y->second)) {
          if (joined->trivial())
            a.when.conditions.erase(*different);
          else
            x->second = *joined;
        } else {
          compatible = false;
        }
      }
      if (compatible) {
        ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(j));
        changed = true;
      } else {
        ++j;
      }
    }
  if (changed)
    std::ranges::sort(ranges);
  return changed;
}

void SafetyState::initialize(PlaceId storage, InitializedRange range) {
  if (range.zeroed) {
    auto plain = range;
    plain.zeroed = false;
    initialize(storage, std::move(plain));
  }
  if (range.begin == range.end)
    return;
  auto &ranges = memory[storage];
  if (std::ranges::find(ranges, range) != ranges.end())
    return;
  // Exact adjacency is valid for symbolic endpoints as well as constants.
  // Every constituent interval is a must-fact; no gap is filled here.
  bool extended = true;
  while (extended) {
    extended = std::erase_if(ranges, [&](const InitializedRange &old) {
                 if (old.source != range.source || old.zeroed != range.zeroed ||
                     old.when != range.when)
                   return false;
                 if (old.end == range.begin) {
                   range.begin = old.begin;
                   return true;
                 }
                 if (range.end == old.begin) {
                   range.end = old.end;
                   return true;
                 }
                 return false;
               }) != 0;
  }
  if (range.begin.isConstant() && range.end.isConstant()) {
    if (range.begin.constant > range.end.constant)
      return;
    // Coalesce only known adjacent/overlapping intervals, never gaps.
    std::erase_if(ranges, [&](const InitializedRange &old) {
      if (old.source != range.source || old.zeroed != range.zeroed ||
          old.when != range.when || !old.begin.isConstant() ||
          !old.end.isConstant() || old.end.constant < range.begin.constant ||
          range.end.constant < old.begin.constant)
        return false;
      range.begin.constant = std::min(range.begin.constant, old.begin.constant);
      range.end.constant = std::max(range.end.constant, old.end.constant);
      return true;
    });
  }
  // Losing initialization evidence is safe; the later read stays unresolved.
  if (ranges.size() < MaxInitializedRanges) {
    ranges.push_back(range);
    mergeInitializedConditions(ranges);
    std::ranges::sort(ranges);
  }
}

void SafetyState::forgetZeros() {
  termination.clear();
  for (auto &[storage, ranges] : memory) {
    (void)storage;
    std::erase_if(ranges, [](const auto &range) { return range.zeroed; });
  }
}
void SafetyState::copyMemory(PlaceId source, PlaceId destination) {
  if (source == destination)
    return;
  if (const auto position = positions.find(source); position != positions.end())
    positions.insert_or_assign(destination, position->second);
  else
    positions.erase(destination);
  if (const auto object = objects.find(source); object != objects.end())
    objects[destination] = object->second;
  else
    objects.erase(destination);
  const auto it = memory.find(source);
  if (it == memory.end())
    memory.erase(destination);
  else
    memory[destination] = it->second;
  if (const auto witness = termination.find(source);
      witness != termination.end())
    termination[destination] = witness->second;
  else
    termination.erase(destination);
}
void SafetyState::forget(PlaceId place) {
  containers.erase(place);
  objectTypes.erase(place);
  writtenStorage.erase(place);
  termination.erase(place);
  replacedPointers.erase(place);
  invalidatedPointers.erase(place);
  accessible.erase(place);
  positions.erase(place);
  std::erase_if(positions, [&](const auto &entry) {
    return entry.second.storage == place;
  });
  objects.erase(place);
  initialized.erase(place);
  pointers.erase(place);
  deferred.erase(place);
  memory.erase(place);
  forgetDependency(place);
}

void SafetyState::forgetDependency(PlaceId place) {
  for (auto &[storage, witnesses] : termination) {
    (void)storage;
    std::erase_if(witnesses, [&](const auto &witness) {
      return witness.begin.place == place || witness.zero.place == place ||
             witness.when.dependsOn(place);
    });
  }
  std::erase_if(termination,
                [](const auto &entry) { return entry.second.empty(); });
  std::erase_if(accessible,
                [&](const auto &entry) { return entry.second.place == place; });
  std::erase_if(positions, [&](const auto &entry) {
    return entry.second.offset.place == place ||
           (entry.second.extent && entry.second.extent->place == place);
  });
  for (auto &path : paths)
    path.drop(place);
  for (auto &[storage, ranges] : memory) {
    (void)storage;
    std::erase_if(ranges, [&](const InitializedRange &range) {
      return range.begin.place == place || range.end.place == place ||
             range.when.dependsOn(place);
    });
  }
}

static bool guardsDisjoint(const PlaceGuard &a, const PlaceGuard &b) {
  for (const auto &[key, fact] : a.conditions)
    if (const auto found = b.conditions.find(key);
        found != b.conditions.end() && fact.disjointFrom(found->second))
      return true;
  for (const auto &[pair, equal] : a.pointers)
    if (const auto actual = b.pointerFact(pair.first, pair.second);
        actual && *actual != equal)
      return true;
  for (const auto &predicate : a.integers) {
    const auto value = predicate.evaluate([&](PlaceId place, IntegerType type) {
      const auto found = b.conditions.find(place);
      return found != b.conditions.end() && !found->second.isPointer()
                 ? found->second.inType(type)
                 : IntegerRange::full(type);
    });
    if (value && !*value)
      return true;
  }
  return false;
}

void SafetyState::refinePaths(const PlaceGuard &guard) {
  if (paths.empty()) {
    paths.push_back(guard);
  } else {
    std::erase_if(paths, [&](const auto &path) {
      return guardsDisjoint(path, guard) || guardsDisjoint(guard, path);
    });
    for (auto &path : paths)
      path.conjoin(guard); // A weaker path condition is conservative.
  }
  if (std::ranges::any_of(paths,
                          [](const auto &path) { return path.trivial(); }))
    paths.clear();
}

bool SafetyState::join(const SafetyState &other, const PlaceGuard &left,
                       const PlaceGuard &right) {
  bool changed = other.havoc && !havoc;
  changed |= containers.join(other.containers);
  for (auto &[storage, type] : objectTypes) {
    const auto found = other.objectTypes.find(storage);
    if (type != "?" &&
        (found == other.objectTypes.end() || found->second != type)) {
      type = "?";
      changed = true;
    }
  }
  for (const auto &[storage, type] : other.objectTypes) {
    (void)type;
    if (!objectTypes.contains(storage)) {
      objectTypes.emplace(storage, "?");
      changed = true;
    }
  }
  const auto written = writtenStorage.size();
  writtenStorage.insert(other.writtenStorage.begin(),
                        other.writtenStorage.end());
  changed |= writtenStorage.size() != written;
  for (auto &[storage, witnesses] : termination) {
    const auto found = other.termination.find(storage);
    changed |= std::erase_if(witnesses, [&](const auto &witness) {
                 return found == other.termination.end() ||
                        std::ranges::find(found->second, witness) ==
                            found->second.end();
               }) != 0;
  }
  changed |= std::erase_if(termination, [](const auto &entry) {
               return entry.second.empty();
             }) != 0;
  const auto replacements = replacedPointers.size();
  replacedPointers.insert(other.replacedPointers.begin(),
                          other.replacedPointers.end());
  changed |= replacements != replacedPointers.size();
  const auto invalidations = invalidatedPointers.size();
  invalidatedPointers.insert(other.invalidatedPointers.begin(),
                             other.invalidatedPointers.end());
  changed |= invalidations != invalidatedPointers.size();
  changed |=
      std::erase_if(accessible, [&](const auto &entry) {
        const auto found = other.accessible.find(entry.first);
        return found == other.accessible.end() || found->second != entry.second;
      }) != 0;
  changed |=
      std::erase_if(positions, [&](const auto &entry) {
        const auto found = other.positions.find(entry.first);
        return found == other.positions.end() || found->second != entry.second;
      }) != 0;
  havoc |= other.havoc;
  auto leftPaths = paths.empty() ? std::vector<PlaceGuard>{left} : paths;
  auto rightPaths =
      other.paths.empty() ? std::vector<PlaceGuard>{right} : other.paths;
  auto mergedPaths = leftPaths;
  mergedPaths.insert(mergedPaths.end(), rightPaths.begin(), rightPaths.end());
  std::ranges::sort(mergedPaths);
  mergedPaths.erase(std::ranges::unique(mergedPaths).begin(),
                    mergedPaths.end());
  if (mergedPaths.size() > MaxGuardConjuncts ||
      std::ranges::any_of(mergedPaths,
                          [](const auto &path) { return path.trivial(); }))
    mergedPaths.clear();
  changed |= paths != mergedPaths;
  paths = std::move(mergedPaths);
  changed |=
      std::erase_if(objects, [&](const auto &entry) {
        const auto found = other.objects.find(entry.first);
        return found == other.objects.end() || found->second != entry.second;
      }) != 0;
  const auto previousDeferred = deferred.size();
  deferred.insert(other.deferred.begin(), other.deferred.end());
  changed |= previousDeferred != deferred.size();
  changed |= std::erase_if(initialized, [&](PlaceId p) {
               return !other.initialized.contains(p);
             }) != 0;
  changed |= std::erase_if(pointers, [&](PlaceId p) {
               return !other.pointers.contains(p);
             }) != 0;
  std::vector<PlaceId> storage;
  storage.reserve(memory.size() + other.memory.size());
  for (const auto &[place, ranges] : memory) {
    (void)ranges;
    storage.push_back(place);
  }
  for (const auto &[place, ranges] : other.memory) {
    (void)ranges;
    storage.push_back(place);
  }
  std::ranges::sort(storage);
  storage.erase(std::ranges::unique(storage).begin(), storage.end());
  for (const auto place : storage) {
    const auto ours = memory.find(place);
    const auto theirs = other.memory.find(place);
    const std::vector<InitializedRange> empty;
    const auto &aRanges = ours == memory.end() ? empty : ours->second;
    const auto &bRanges = theirs == other.memory.end() ? empty : theirs->second;
    std::vector<InitializedRange> common;
    for (const auto &a : aRanges) {
      for (const auto &b : bRanges) {
        if (a.source != b.source || a.zeroed != b.zeroed || a.when != b.when)
          continue;
        if (a == b) {
          common.push_back(a);
        } else if (a.begin.isConstant() && a.end.isConstant() &&
                   b.begin.isConstant() && b.end.isConstant()) {
          const auto first = std::max(a.begin.constant, b.begin.constant);
          const auto last = std::min(a.end.constant, b.end.constant);
          if (first < last)
            common.push_back({.begin = Affine::ofConstant(first),
                              .end = Affine::ofConstant(last),
                              .when = a.when,
                              .source = a.source,
                              .zeroed = a.zeroed});
        }
      }
    }
    // A fact missing on another edge survives only with a guard that edge
    // demonstrably excludes. Truncated path facts can lose precision, but
    // disjointness is still positive evidence for this implication.
    const auto conditional = [&](const auto &ranges, const auto &incoming,
                                 const auto &opposite) {
      const auto excludes = [&](const PlaceGuard &guard) {
        return std::ranges::all_of(opposite, [&](const auto &path) {
          return guardsDisjoint(guard, path) || guardsDisjoint(path, guard);
        });
      };
      for (auto range : ranges) {
        // RFC 0020: a fact already established on both edges needs no new
        // branch premise. Re-adding stricter copies both wastes disjointness
        // work and consumes the finite initialized-range budget.
        if (std::ranges::find(common, range) != common.end())
          continue;
        if (excludes(range.when)) {
          common.push_back(std::move(range));
          continue;
        }
        for (const auto &path : incoming) {
          auto fact = range;
          if (fact.when.size() + path.size() > MaxGuardConjuncts ||
              guardsDisjoint(fact.when, path))
            continue;
          fact.when.conjoin(path);
          if (excludes(fact.when))
            common.push_back(std::move(fact));
        }
      }
    };
    conditional(aRanges, leftPaths, rightPaths);
    conditional(bRanges, rightPaths, leftPaths);
    mergeInitializedConditions(common);
    std::ranges::sort(common);
    common.erase(std::ranges::unique(common).begin(), common.end());
    if (common.size() > MaxInitializedRanges)
      common.resize(MaxInitializedRanges);
    if (common.empty()) {
      changed |= memory.erase(place) != 0;
    } else if (common != aRanges) {
      changed = true;
      memory[place] = std::move(common);
    }
  }
  return changed;
}

std::string safetyJsonString(std::string_view value) {
  static constexpr std::string_view Hex = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() + 2);
  result += '"';
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto begin = i;
    while (i < value.size()) {
      const auto plain = static_cast<unsigned char>(value[i]);
      if (plain < 0x20 || plain >= 0x80 || plain == '"' || plain == '\\')
        break;
      ++i;
    }
    result.append(value.substr(begin, i - begin));
    if (i == value.size())
      break;
    const auto byte = static_cast<unsigned char>(value[i]);
    if (byte >= 0x80) {
      unsigned length = 0;
      if (byte >= 0xc2 && byte <= 0xdf)
        length = 2;
      else if (byte >= 0xe0 && byte <= 0xef)
        length = 3;
      else if (byte >= 0xf0 && byte <= 0xf4)
        length = 4;
      bool valid = length != 0 && i + length <= value.size();
      for (unsigned j = 1; valid && j < length; ++j)
        valid = (static_cast<unsigned char>(value[i + j]) & 0xc0U) == 0x80U;
      if (valid) {
        const auto next = static_cast<unsigned char>(value[i + 1]);
        valid = (byte != 0xe0 || next >= 0xa0) &&
                (byte != 0xed || next < 0xa0) &&
                (byte != 0xf0 || next >= 0x90) && (byte != 0xf4 || next < 0x90);
      }
      if (valid) {
        result.append(value.substr(i, length));
        i += length - 1;
        continue;
      }
    }
    if (byte == '"' || byte == '\\') {
      result += '\\';
      result += value[i];
    } else if (byte < 0x20 || byte >= 0x80) {
      result += "\\u00";
      result += Hex[byte >> 4U];
      result += Hex[byte & 15U];
    } else {
      result += value[i];
    }
  }
  result += '"';
  return result;
}

} // namespace weavec::core
