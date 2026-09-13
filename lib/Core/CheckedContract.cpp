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

std::optional<CheckedRequirement>
joinContainerOutput(const CheckedRequirement &first,
                    const CheckedRequirement &second) {
  if (first.kind != CheckedRequirementKind::ContainerDerived ||
      second.kind != CheckedRequirementKind::ContainerDerived ||
      first.path != second.path || first.family != second.family ||
      first.on != second.on || first.when != second.when || first.ifNonNull ||
      second.ifNonNull)
    return std::nullopt;
  std::set<SummaryPath> sources{first.other, second.other};
  for (const auto *post : {&first, &second}) {
    if (post->begin.path)
      sources.insert(*post->begin.path);
    if (post->end.path)
      sources.insert(*post->end.path);
  }
  auto result = first;
  result.begin = result.end = {};
  result.other = {};
  if (sources.size() > 3) {
    result.kind = CheckedRequirementKind::Container;
    return result;
  }
  auto source = sources.begin();
  result.other = *source++;
  if (source != sources.end())
    result.begin = PathAffine::ofPath(*source++);
  if (source != sources.end())
    result.end = PathAffine::ofPath(*source);
  return result;
}

CheckedRequirements::CheckedRequirements(
    std::initializer_list<CheckedRequirement> entries) {
  assign(Set(entries));
}

const CheckedRequirements::Set &CheckedRequirements::entries() const {
  static const Set Empty;
  return values ? *values : Empty;
}

CheckedRequirements::Set &CheckedRequirements::writable() {
  if (!values)
    values = std::make_shared<Set>();
  else if (values.use_count() != 1)
    values = std::make_shared<Set>(*values);
  return *values;
}

std::pair<CheckedRequirements::ConstIterator, bool>
CheckedRequirements::insert(CheckedRequirement entry) {
  const auto found = entries().lower_bound(entry);
  if (found != end() && !(entry < *found))
    return {found, false};
  // A unique set keeps its insertion hint. Detachment creates a different
  // tree, so locate the hint there before consuming the entry.
  const bool keepHint = values && values.use_count() == 1;
  auto &target = writable();
  const auto hint = keepHint ? found : target.lower_bound(entry);
  return {target.insert(hint, std::move(entry)), true};
}

void CheckedRequirements::assign(Set entries) {
  values =
      entries.empty() ? nullptr : std::make_shared<Set>(std::move(entries));
}

void CheckedRequirements::intersect(const CheckedRequirements &other) {
  if (values == other.values || empty())
    return;
  if (other.empty()) {
    clear();
    return;
  }
  Set generalized;
  for (const auto &first : entries())
    if (first.kind == CheckedRequirementKind::ContainerDerived)
      for (const auto &second : other.entries())
        if (const auto joined = joinContainerOutput(first, second))
          generalized.insert(*joined);
  const auto absent = [&](const CheckedRequirement &entry) {
    return !other.contains(entry);
  };
  // Preserve shared storage when the intersection removes nothing.
  if (std::ranges::any_of(entries(), absent))
    std::erase_if(writable(), absent);
  for (const auto &entry : generalized)
    if (size() < MaxSafetyRequirements)
      insert(entry);
}

static constexpr std::array<std::string_view, 24> Kinds{
    "valid",
    "extent",
    "initialized",
    "release",
    "separated",
    "writable",
    "terminated",
    "copied",
    "sum-fits",
    "zeroed",
    "position",
    "progress",
    "object-type",
    "container",
    "container-separated",
    "container-derived",
    "container-fresh",
    "container-tail",
    "format-arguments",
    "argument-list",
    "argument-list-consumed",
    "terminated-within",
    "standard-stream",
    "union-member"};

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
void CheckedContract::noteCaseInput(const SummaryPath &path) {
  if (path.isResult() || path.steps.size() > MaxHeapPathDepth)
    return;
  caseInputs.insert(path);
  if (caseInputs.size() > 64)
    caseInputs.erase(std::prev(caseInputs.end()));
}
void CheckedContract::require(CheckedRequirement requirement) {
  if (requirements.size() != MaxSafetyRequirements)
    requirements.insert(std::move(requirement));
  else if (!requirements.contains(requirement))
    limited = true;
}
void CheckedContract::establish(CheckedRequirement requirement) {
  if (establishes.size() != MaxSafetyRequirements)
    establishes.insert(std::move(requirement));
  else if (!establishes.contains(requirement))
    limited = true;
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
  for (const auto &path : other.caseInputs)
    noteCaseInput(path);
  if (requirements != other.requirements)
    for (const auto &requirement : other.requirements)
      if (!requirements.contains(requirement))
        require(requirement);
  if (establishes != other.establishes) {
    std::vector<CheckedRequirement> retirements;
    const auto collect = [&](const CheckedRequirements &facts) {
      for (const auto &post : facts)
        if (post.kind == CheckedRequirementKind::ArgumentListConsumed)
          retirements.push_back(post);
    };
    collect(establishes);
    collect(other.establishes);
    establishes.intersect(other.establishes);
    for (auto &post : retirements)
      establish(std::move(post));
  }
  obligations.join(other.obligations);
}

} // namespace weavec::core
