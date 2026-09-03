//===- Raw.cpp - Raw pointer tracking -------------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Raw.h"

#include <utility>

namespace weavec::core {

bool RawTracker::markRaw(PlaceId place, RawReason reason,
                         SourceLocation location, std::optional<PlaceId> via) {
  return markRaw(place, RawRecord{.reason = reason,
                                  .location = std::move(location),
                                  .via = via,
                                  .detail = {}});
}

bool RawTracker::markRaw(PlaceId place, const RawRecord &record) {
  return raw.try_emplace(place, record).second;
}

void RawTracker::clear(PlaceId place) {
  raw.erase(place);
}

std::optional<RawRecord> RawTracker::rawAt(PlaceId place) const {
  const auto it = raw.find(place);
  if (it == raw.end())
    return std::nullopt;
  return it->second;
}

bool RawTracker::join(const RawTracker &other) {
  bool changed = false;
  for (const auto &[place, record] : other.raw)
    changed |= raw.try_emplace(place, record).second;
  return changed;
}

std::vector<PlaceId> RawTracker::rawPlaces() const {
  std::vector<PlaceId> result;
  result.reserve(raw.size());
  for (const auto &[place, record] : raw)
    result.push_back(place);
  return result;
}

std::string_view toString(RawReason reason) noexcept {
  switch (reason) {
  case RawReason::IntegerCast:
    return "integer-cast";
  case RawReason::Declared:
    return "declared";
  case RawReason::LoadedThroughRaw:
    return "loaded-through-raw";
  case RawReason::Callee:
    return "callee";
  case RawReason::UnknownCallee:
    return "unknown-callee";
  }
  return "<invalid>";
}

} // namespace weavec::core
