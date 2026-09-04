//===- Moves.cpp - Move / deinitialization tracking -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Moves.h"

#include <utility>

namespace weavec::core {

bool ElementWitness::matches(const ElementWitness &other) const noexcept {
  if (kind == Kind::Whole || other.kind == Kind::Whole)
    return true;
  if (kind != other.kind)
    return false;
  switch (kind) {
  case Kind::Constant:
    return constant == other.constant;
  case Kind::Variable:
    return variable == other.variable;
  case Kind::Whole:
  case Kind::Unknown:
    return false;
  }
  return false;
}

std::optional<MoveRecord>
MoveTracker::markMoved(PlaceId place, MoveReason reason,
                       SourceLocation location, std::optional<PlaceId> via,
                       ElementWitness element, std::string family,
                       bool ownValue, PlaceGuard guard) {
  MoveRecord record{.reason = reason,
                    .location = std::move(location),
                    .via = via,
                    .element = element,
                    .family = std::move(family),
                    .ownValue = ownValue,
                    .guard = std::move(guard)};
  auto [it, inserted] = moved.try_emplace(place, record);
  if (inserted)
    return std::nullopt;
  if (it->second.element.matches(element)) {
    // Already moved: report the earlier move but keep the original record so
    // later diagnostics point at the first offending site. A second consume
    // under a guard the first did not have is still a second consume; the
    // place is now moved whenever either happened.
    if (!it->second.guard.trivial())
      it->second.guard.join(record.guard);
    return it->second;
  }
  // Another element of the same summarised place: the most recent one is
  // what later accesses in the same iteration name (RFC 0006).
  it->second = std::move(record);
  return std::nullopt;
}

void MoveTracker::reinitialize(PlaceId place, ElementWitness element) {
  const auto it = moved.find(place);
  if (it == moved.end())
    return;
  if (element.isWhole() || it->second.element.matches(element))
    moved.erase(it);
}

std::optional<MoveRecord> MoveTracker::movedAt(PlaceId place,
                                               ElementWitness element) const {
  const auto it = moved.find(place);
  if (it == moved.end() || !it->second.element.matches(element))
    return std::nullopt;
  return it->second;
}

std::optional<MoveRecord> MoveTracker::recordOf(PlaceId place) const {
  const auto it = moved.find(place);
  if (it == moved.end())
    return std::nullopt;
  return it->second;
}

void MoveTracker::forgetWitness(PlaceId variable) {
  for (auto &[place, record] : moved) {
    if (record.element.kind == ElementWitness::Kind::Variable &&
        record.element.variable == variable)
      record.element = ElementWitness::unknown();
  }
}

bool MoveTracker::join(const MoveTracker &other) {
  bool changed = false;
  for (const auto &[place, record] : other.moved) {
    auto [it, inserted] = moved.try_emplace(place, record);
    if (inserted) {
      changed = true;
      continue;
    }
    // Both sides moved the place: it is moved when either guard holds.
    changed |= it->second.guard.join(record.guard);
    // A record that may be the caller's value on either path is the
    // caller's after the join (RFC 0008, *Replaced values*).
    if (it->second.ownValue && !record.ownValue) {
      it->second.ownValue = false;
      changed = true;
    }
    if (it->second.element == record.element)
      continue;
    // Both paths moved the place but not the same element. A whole-place
    // move on either side covers every element; otherwise the element is
    // unknown.
    ElementWitness element = it->second.element;
    if (record.element.isWhole())
      element = ElementWitness::whole();
    else if (!element.isWhole())
      element = ElementWitness::unknown();
    if (element != it->second.element) {
      it->second.element = element;
      changed = true;
    }
  }
  return changed;
}

std::vector<PlaceId> MoveTracker::learn(PlaceId place, const ValueFact &fact) {
  std::vector<PlaceId> refuted;
  for (auto it = moved.begin(); it != moved.end();) {
    if (it->second.guard.learn(place, fact) == GuardRefinement::Refuted) {
      refuted.push_back(it->first);
      it = moved.erase(it);
      continue;
    }
    ++it;
  }
  return refuted;
}

void MoveTracker::dropGuardsOn(PlaceId place) {
  for (auto &[movedPlace, record] : moved)
    record.guard.drop(place);
}

void MoveTracker::setGuard(PlaceId place, PlaceGuard guard) {
  const auto it = moved.find(place);
  if (it != moved.end())
    it->second.guard = std::move(guard);
}

std::vector<PlaceId> MoveTracker::movedPlaces() const {
  std::vector<PlaceId> result;
  result.reserve(moved.size());
  for (const auto &[place, record] : moved)
    result.push_back(place);
  return result;
}

std::string_view toString(MoveReason reason) noexcept {
  switch (reason) {
  case MoveReason::Moved:
    return "moved";
  case MoveReason::Freed:
    return "freed";
  case MoveReason::Uninitialized:
    return "uninitialized";
  case MoveReason::Released:
    return "released";
  }
  return "<invalid>";
}

} // namespace weavec::core
