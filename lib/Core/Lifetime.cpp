//===- Lifetime.cpp - Lifetime regions and outlives constraints -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Lifetime.h"

#include <utility>

namespace weavec::core {

LifetimeConstraints::LifetimeConstraints() {
  names.emplace_back("'static");
}

LifetimeId LifetimeConstraints::fresh(std::string debugName) {
  const auto id = static_cast<std::uint32_t>(names.size());
  if (debugName.empty())
    debugName = "'" + std::to_string(id);
  names.push_back(std::move(debugName));
  return LifetimeId{id};
}

void LifetimeConstraints::addOutlives(LifetimeId longer, LifetimeId shorter) {
  if (longer == shorter || longer.isStatic())
    return;
  edges[longer.value].insert(shorter.value);
}

bool LifetimeConstraints::outlives(LifetimeId longer,
                                   LifetimeId shorter) const {
  if (longer == shorter || longer.isStatic())
    return true;
  if (shorter.isStatic())
    return false;

  // Depth-first search over the outlives graph. Graphs are small (per
  // function), so an explicit closure is not worth maintaining yet.
  std::vector<std::uint32_t> stack{longer.value};
  std::unordered_set<std::uint32_t> visited{longer.value};
  while (!stack.empty()) {
    const std::uint32_t current = stack.back();
    stack.pop_back();
    const auto it = edges.find(current);
    if (it == edges.end())
      continue;
    for (const std::uint32_t next : it->second) {
      if (next == shorter.value)
        return true;
      if (visited.insert(next).second)
        stack.push_back(next);
    }
  }
  return false;
}

std::string LifetimeConstraints::name(LifetimeId id) const {
  if (id.value < names.size())
    return names[id.value];
  return "'?" + std::to_string(id.value);
}

} // namespace weavec::core
