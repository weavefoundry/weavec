//===- SafetyCallPath.cpp - Shared provenance (RFC 0020) ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/SafetyCallPath.h"

#include "weavec/Core/Safety.h"

#include <algorithm>
#include <iterator>

namespace weavec::core {

SafetyCallPath::SafetyCallPath(std::vector<SourceLocation> locations) {
  if (!locations.empty())
    storage = std::make_shared<Storage>(std::move(locations), false);
}

SafetyCallPath::SafetyCallPath(std::initializer_list<SourceLocation> locations)
    : SafetyCallPath(std::vector<SourceLocation>(locations)) {}

const std::vector<SourceLocation> &SafetyCallPath::entries() const {
  static const std::vector<SourceLocation> Empty;
  return storage ? storage->locations : Empty;
}

std::vector<SourceLocation> &SafetyCallPath::writable() {
  if (!storage)
    storage = std::make_shared<Storage>();
  else if (storage.use_count() != 1)
    storage = std::make_shared<Storage>(*storage);
  storage->normalized = false;
  return storage->locations;
}

void SafetyCallPath::pushBack(SourceLocation location) {
  writable().push_back(std::move(location));
}

void SafetyCallPath::resize(std::size_t count) {
  writable().resize(count);
}

void SafetyCallPath::insert(ConstIterator position, SourceLocation location) {
  const auto offset = std::distance(begin(), position);
  auto &locations = writable();
  locations.insert(std::next(locations.begin(), offset), std::move(location));
}

void SafetyCallPath::normalize() {
  if (!storage || storage->normalized)
    return;
  // Preserve raw copies, but move strings when this edited path is unique.
  auto pending = std::move(writable());
  std::vector<SourceLocation> chain;
  chain.reserve(std::min(pending.size(), MaxSafetyCallDepth + 1));
  for (auto &call : pending) {
    call.opaque = 0;
    const auto repeated = std::ranges::find(chain, call);
    if (repeated != chain.end())
      chain.erase(std::next(repeated), chain.end());
    else
      chain.push_back(std::move(call));
  }
  if (chain.size() > MaxSafetyCallDepth) {
    auto origin = std::move(chain.back());
    chain.resize(MaxSafetyCallDepth);
    chain.back() = std::move(origin);
  }
  storage->locations = std::move(chain);
  storage->normalized = true;
}

bool operator==(const SafetyCallPath &left, const SafetyCallPath &right) {
  return left.storage == right.storage || left.entries() == right.entries();
}

} // namespace weavec::core
