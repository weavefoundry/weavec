//===- DataflowGuardCompleteness.cpp - Numeric guard coverage (RFC 0017) --===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

#include <algorithm>

namespace weavec::analysis {

bool FunctionDataflow::integerGuardComplete(
    const core::PlaceGuard &guard, const core::AnalysisState &state,
    std::optional<core::PlaceId> exclude) {
  if (state.numericConditionsIncomplete)
    return false;
  for (const auto &predicate : state.numericConditions.integers) {
    // Only the index of an eligible loop may be quantified out of a must
    // requirement. The caller establishes eligibility before excluding it.
    if (exclude && predicate.dependsOn(*exclude))
      continue;
    if (std::ranges::binary_search(guard.integers, predicate))
      continue;
    // pathGuard() shares a capacity between scalar and numeric premises. It
    // may omit a predicate because of scalar facts that did not fit either.
    // Only facts actually retained in this guard can justify that omission;
    // consulting integerRangeAt/state relations would prove the wrong guard.
    const auto implied = predicate.evaluate(
        [&guard](core::PlaceId place, core::IntegerType type) {
          const auto fact = guard.conditions.find(place);
          return fact != guard.conditions.end() && !fact->second.isPointer()
                     ? fact->second.inType(type)
                     : core::IntegerRange::full(type);
        });
    if (!implied || !*implied)
      return false;
  }
  return true;
}

bool FunctionDataflow::summaryGuardComplete(
    const core::PlaceGuard &guard, const core::PathGuard &projectedGuard) {
  // Several cells can project to one predicate: under an alias context,
  // *a and *b both hold the last written value. Count each original premise
  // as covered when its projection survives, even if it is deduplicated.
  const auto covered = [&](const core::PlaceGuard &premise) {
    const auto projected = summaryGuardOf(premise);
    if (projected.size() != 1)
      return false;
    for (const auto &[path, fact] : projected.conditions) {
      const auto retained = projectedGuard.conditions.find(path);
      if (retained == projectedGuard.conditions.end() ||
          !retained->second.implies(fact))
        return false;
    }
    for (const auto &[pair, equal] : projected.pointers)
      if (projectedGuard.pointerFact(pair.first, pair.second) != equal)
        return false;
    return std::ranges::all_of(projected.integers, [&](const auto &predicate) {
      return std::ranges::binary_search(projectedGuard.integers, predicate);
    });
  };
  bool guardComplete = true;
  for (const auto &[place, fact] : guard.conditions) {
    core::PlaceGuard premise;
    premise.conditions.emplace(place, fact);
    guardComplete &= covered(premise);
  }
  for (const auto &[pair, equal] : guard.pointers) {
    core::PlaceGuard premise;
    premise.pointers.emplace(pair, equal);
    guardComplete &= covered(premise);
  }
  for (const auto &predicate : guard.integers) {
    core::PlaceGuard premise;
    premise.integers.push_back(predicate);
    guardComplete &= covered(premise);
  }
  return guardComplete;
}

} // namespace weavec::analysis
