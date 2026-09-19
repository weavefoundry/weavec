//===- Moves.cpp - Move / deinitialization tracking -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Moves.h"

#include <algorithm>
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
                       bool ownValue, PlaceGuard guard, MoveOrigin origin) {
  MoveRecord record{.reason = reason,
                    .location = std::move(location),
                    .via = via,
                    .element = element,
                    .family = std::move(family),
                    .ownValue = ownValue,
                    .guard = std::move(guard),
                    // RFC 0030 §5.1: the unknown-callee default holds on
                    // some paths only.
                    .allPaths = !origin.unknownOrigin,
                    .conditional = origin.conditional || origin.lossy,
                    .lossy = origin.lossy,
                    .unknownOrigin = origin.unknownOrigin};
  return copyRecord(place, std::move(record));
}

const MoveTracker::Records &MoveTracker::view() const {
  static const Records none;
  return records ? *records : none;
}

MoveTracker::Records &MoveTracker::edit() {
  if (!records)
    records = std::make_shared<Records>();
  else if (records.use_count() > 1)
    records = std::make_shared<Records>(*records);
  return *records;
}

std::optional<MoveRecord> MoveTracker::copyRecord(PlaceId place,
                                                  MoveRecord record) {
  // Already moved with no guard to join: nothing changes, and the records
  // stay shared.
  if (const auto found = view().find(place);
      found != view().end() && found->second.element.matches(record.element) &&
      found->second.guard.trivial())
    return found->second;
  auto [it, inserted] = edit().try_emplace(place, record);
  if (inserted)
    return std::nullopt;
  if (it->second.element.matches(record.element)) {
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
  const auto it = view().find(place);
  if (it == view().end())
    return;
  if (element.isWhole() || it->second.element.matches(element))
    edit().erase(place);
}

std::optional<MoveRecord> MoveTracker::movedAt(PlaceId place,
                                               ElementWitness element) const {
  const auto it = view().find(place);
  if (it == view().end() || !it->second.element.matches(element))
    return std::nullopt;
  return it->second;
}

std::optional<MoveRecord> MoveTracker::recordOf(PlaceId place) const {
  const auto it = view().find(place);
  if (it == view().end())
    return std::nullopt;
  return it->second;
}

const MoveRecord *MoveTracker::find(PlaceId place) const {
  const auto it = view().find(place);
  return it == view().end() ? nullptr : &it->second;
}

void MoveTracker::forgetWitness(PlaceId variable) {
  const auto names = [variable](const MoveRecord &record) {
    return record.element.kind == ElementWitness::Kind::Variable &&
           record.element.variable == variable;
  };
  if (std::none_of(view().begin(), view().end(),
                   [&](const auto &entry) { return names(entry.second); }))
    return;
  for (auto &[place, record] : edit()) {
    if (record.element.kind == ElementWitness::Kind::Variable &&
        record.element.variable == variable)
      record.element = ElementWitness::unknown();
  }
}

void MoveTracker::settleConditional(PlaceId place) {
  const auto it = view().find(place);
  if (it != view().end() && !it->second.lossy && it->second.conditional)
    edit()[place].conditional = false;
}

bool MoveTracker::markUnknown(PlaceId place, const SourceLocation &location,
                              std::string_view origin) {
  if (view().contains(place))
    return false;
  MoveRecord record{.reason = MoveReason::Freed,
                    .location = SourceLocation{.file = {},
                                               .line = location.line,
                                               .column = location.column,
                                               .opaque = location.opaque},
                    .via = std::nullopt,
                    .element = ElementWitness::whole(),
                    .family = {},
                    .ownValue = false,
                    .guard = {},
                    .allPaths = false,
                    .conditional = false,
                    .lossy = false,
                    .unknownOrigin = true,
                    .origin = std::string(origin)};
  return edit().try_emplace(place, std::move(record)).second;
}

bool MoveTracker::eraseUnknown(PlaceId place) {
  const auto it = view().find(place);
  if (it == view().end() || !it->second.unknownOrigin)
    return false;
  edit().erase(place);
  return true;
}

void MoveTracker::reaffirm(PlaceId place, PlaceGuard guard) {
  const auto it = view().find(place);
  if (it == view().end() || it->second.unknownOrigin)
    return;
  MoveRecord &record = edit()[place];
  record.guard = std::move(guard);
  record.allPaths = true;
  record.conditional = false;
  record.lossy = false;
}

bool MoveTracker::join(const MoveTracker &other) {
  // The same records on both sides (copies of one state): nothing changes,
  // and equal records are shared from now on.
  if (records == other.records)
    return false;
  if (other.view() == view()) {
    records = other.records;
    return false;
  }
  bool changed = false;
  const Records &theirs = other.view();
  Records &moved = edit();
  // RFC 0030 §3.1: a record this side has and the other lacks reached here
  // on some paths only.
  for (auto &[place, record] : moved) {
    if (record.allPaths && !theirs.contains(place)) {
      record.allPaths = false;
      changed = true;
    }
  }
  for (const auto &[place, record] : theirs) {
    auto [it, inserted] = moved.try_emplace(place, record);
    if (inserted) {
      it->second.allPaths = false;
      changed = true;
      continue;
    }
    MoveRecord &mine = it->second;
    // RFC 0030 §3.1 (amended in S3): a release of the function's own value
    // (RFC 0008, `ownValue`) and an unknown-origin record of the caller's
    // value describe different values. All the join knows of the caller's
    // value is that unknown code had it, so the unknown record stands, on
    // some paths.
    if (mine.unknownOrigin != record.unknownOrigin) {
      const MoveRecord &known = mine.unknownOrigin ? record : mine;
      const MoveRecord &unknown = mine.unknownOrigin ? mine : record;
      if (known.ownValue && !unknown.ownValue) {
        if (!mine.unknownOrigin) {
          mine = record;
          mine.allPaths = false;
          changed = true;
        } else if (mine.allPaths) {
          mine.allPaths = false;
          changed = true;
        }
        continue;
      }
    }
    // A known record joined into an unknown one brings its own reason,
    // position and names (only a known record is ever diagnosed); the bits,
    // the guard and the element join as below.
    if (mine.unknownOrigin && !record.unknownOrigin) {
      MoveRecord unknown = std::move(mine);
      mine = record;
      mine.allPaths = false;
      mine.conditional = mine.conditional || unknown.conditional;
      mine.lossy = mine.lossy || unknown.lossy;
      (void)mine.guard.join(unknown.guard);
      if (mine.ownValue && !unknown.ownValue)
        mine.ownValue = false;
      if (unknown.element.isWhole())
        mine.element = ElementWitness::whole();
      else if (!mine.element.isWhole() && mine.element != unknown.element)
        mine.element = ElementWitness::unknown();
      changed = true;
      continue;
    }
    const bool allPaths = mine.allPaths && record.allPaths &&
                          mine.unknownOrigin == record.unknownOrigin;
    const bool conditional = mine.conditional || record.conditional;
    const bool lossy = mine.lossy || record.lossy;
    const bool unknownOrigin = mine.unknownOrigin && record.unknownOrigin;
    if (mine.allPaths != allPaths || mine.conditional != conditional ||
        mine.lossy != lossy || mine.unknownOrigin != unknownOrigin) {
      mine.allPaths = allPaths;
      mine.conditional = conditional;
      mine.lossy = lossy;
      mine.unknownOrigin = unknownOrigin;
      changed = true;
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
  // Every guard learns the fact, a trivial one included, except a record of
  // unknown origin's (RFC 0030 §5.1): it is never diagnosed, so a guard could
  // only let a later test drop it, and keeping it is sound. Its guard stays
  // trivial, which keeps the guard scans short.
  if (std::all_of(view().begin(), view().end(),
                  [](const auto &entry) { return entry.second.unknownOrigin; }))
    return refuted;
  Records &moved = edit();
  for (auto it = moved.begin(); it != moved.end();) {
    if (it->second.unknownOrigin) {
      ++it;
      continue;
    }
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
  // Read first: the records stay shared when no guard names `place`.
  if (std::none_of(view().begin(), view().end(), [place](const auto &entry) {
        return entry.second.guard.dependsOn(place);
      }))
    return;
  for (auto &[movedPlace, record] : edit())
    record.guard.drop(place);
}

void MoveTracker::setGuard(PlaceId place, PlaceGuard guard) {
  if (view().contains(place))
    edit()[place].guard = std::move(guard);
}

std::vector<PlaceId> MoveTracker::movedPlaces() const {
  std::vector<PlaceId> result;
  result.reserve(view().size());
  for (const auto &[place, record] : view())
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
