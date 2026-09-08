//===- CheckedContract.cpp - Sufficient safety contracts (RFC 0018) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Summary.h"

#include <array>
#include <span>

namespace weavec::core {

static constexpr std::array<std::string_view, 6> Kinds{
    "valid", "extent", "initialized", "release", "separated", "writable"};

std::string_view toString(CheckedRequirementKind value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  return index < Kinds.size() ? std::span(Kinds)[index] : "invalid";
}
std::optional<CheckedRequirementKind>
parseCheckedRequirementKind(std::string_view value) {
  for (std::size_t i = 0; i < Kinds.size(); ++i)
    if (Kinds.at(i) == value)
      return static_cast<CheckedRequirementKind>(i);
  return std::nullopt;
}
void CheckedContract::require(CheckedRequirement requirement) {
  if (requirements.contains(requirement))
    return;
  if (requirements.size() == MaxSafetyRequirements)
    limited = true;
  else
    requirements.insert(std::move(requirement));
}
void CheckedContract::establish(CheckedRequirement requirement) {
  if (establishes.contains(requirement))
    return;
  if (establishes.size() == MaxSafetyRequirements)
    limited = true;
  else
    establishes.insert(std::move(requirement));
}
void CheckedContract::join(const CheckedContract &other) {
  if (this == &other)
    return;
  selected |= other.selected;
  if (!other.computed)
    return;
  if (!computed) {
    const bool selection = selected;
    *this = other;
    selected |= selection;
    return;
  }
  limited |= signature != other.signature;
  deferred |= other.deferred;
  limited |= other.limited;
  for (const auto &requirement : other.requirements)
    require(requirement);
  std::erase_if(establishes, [&](const CheckedRequirement &requirement) {
    return !other.establishes.contains(requirement);
  });
  obligations.join(other.obligations);
}

} // namespace weavec::core
