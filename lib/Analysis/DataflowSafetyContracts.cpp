//===- DataflowSafetyContracts.cpp - Checked postconditions (RFC 0019)
//-----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"

using namespace clang;

namespace weavec::analysis {

std::optional<core::PlaceGuard>
FunctionDataflow::checkedGuard(const core::PathGuard &when,
                               const CallExpr &call,
                               const core::AnalysisState &state) {
  // The ordinary translator weakens missing premises for may-effects. A
  // must-fact needs every premise represented or independently discharged.
  for (const auto &[path, fact] : when.conditions) {
    if (path.isParam() && path.isRoot() && path.index < call.getNumArgs()) {
      const auto &argument = *call.getArg(path.index);
      if (argument.getType()->isPointerType()) {
        const auto origin = builder.classifyValue(argument);
        if (origin.kind == ValueOrigin::Kind::Null ||
            origin.kind == ValueOrigin::Kind::Borrow || origin.place)
          continue;
      } else if (argument.getType()->isIntegerType()) {
        const auto actual = scalarFactOf(argument, state);
        if (actual && (actual->implies(fact) || actual->disjointFrom(fact)))
          continue;
        const auto operand = builder.scalarOperand(argument);
        if (operand.constant ||
            (operand.place && !operand.scaled && operand.offset == 0))
          continue;
      }
      return std::nullopt;
    }
    if (!builder.resolveSummaryPath(path, call))
      return std::nullopt;
  }
  for (const auto &[pair, equal] : when.pointers) {
    (void)equal;
    if (!builder.resolveSummaryPath(pair.first, call) ||
        !builder.resolveSummaryPath(pair.second, call))
      return std::nullopt;
  }
  // Typed predicates already have strict, all-leaf translation. Failure must
  // not be converted into an unconditional postcondition.
  if (!when.integers.empty()) {
    const auto numeric = translateIntegerGuard(when, call, state);
    if (!numeric)
      return std::nullopt;
  }
  auto mapped = builder.translateGuard(when, call);
  if (!mapped || !pruneGuard(*mapped, state))
    return std::nullopt;
  return mapped;
}

std::vector<core::InitializedRange> FunctionDataflow::checkedCopyRanges(
    const CheckedMemory &source, const core::Affine &begin,
    const core::Affine &end, const core::AnalysisState &state) {
  std::vector<core::InitializedRange> result;
  if (const auto found = state.safety->memory.find(source.storage);
      found != state.safety->memory.end())
    for (auto range : found->second) {
      if (!pruneGuard(range.when, state))
        continue;
      range.begin = foldAffine(range.begin, state);
      range.end = foldAffine(range.end, state);
      const auto first = foldAffine(begin, state);
      const auto last = foldAffine(end, state);
      if (range.begin.isConstant() && range.end.isConstant() &&
          first.isConstant() && last.isConstant()) {
        range.begin.constant = std::max(range.begin.constant, first.constant);
        range.end.constant = std::min(range.end.constant, last.constant);
        if (range.begin.constant >= range.end.constant)
          continue;
      } else if (first != core::Affine::ofConstant(0) ||
                 !checkedInterval(range.begin, range.end, last, state)) {
        continue;
      }
      result.push_back(std::move(range));
    }
  if (source.input && !state.safety->havoc)
    result.push_back({.begin = begin, .end = end, .source = source.storage});
  return result;
}

void FunctionDataflow::captureCheckedPosts(
    const CallExpr &call, const core::CheckedContract &contract,
    core::AnalysisState &state) {
  auto &posts = checkedPosts[&call];
  posts.clear();
  if (!contract.complete())
    return;
  auto &snapshots = checkedSnapshots[&call];
  for (const auto &[input, saved] : snapshots) {
    (void)input;
    state.dropGuardsOn(saved);
    state.scalars.forget(saved);
    state.relations.forget(saved);
    numericSnapshotExpressions.erase(saved);
  }
  std::set<core::PlaceId> captured;
  const auto snapshot = [&](core::PlaceId input) {
    const auto [slot, inserted] = snapshots.try_emplace(input);
    if (inserted) {
      slot->second = places.create("checked call input");
      snapshotPlaces.insert(slot->second);
    }
    const auto saved = slot->second;
    if (!captured.insert(input).second)
      return saved;
    if (const auto fact = state.scalars.factOf(input)) {
      state.scalars.set(saved, *fact);
    } else if (const auto nullness = nullnessAt(input, state)) {
      if (nullness->state == core::Nullness::Null)
        state.scalars.set(saved, core::ValueFact::of(core::Outcome::Null));
      if (nullness->state == core::Nullness::NonNull)
        state.scalars.set(saved, core::ValueFact::of(core::Outcome::NonNull));
    }
    state.relations.learn(saved, core::Relation::Equal, input);
    if (const auto expression = numericExpressions.find(input);
        expression != numericExpressions.end())
      if (const auto projected = summaryIntegerExpression(expression->second))
        numericSnapshotExpressions.emplace(saved, *projected);
    if (const auto path = stableSummaryPathOf(input))
      snapshotInputPaths[saved] = *path;
    return saved;
  };
  const auto freeze = [&](core::Affine value) {
    value = foldAffine(value, state);
    if (value.place)
      value.place = snapshot(*value.place);
    return value;
  };
  for (const auto &post : contract.establishes) {
    if (post.kind != core::CheckedRequirementKind::Initialized &&
        post.kind != core::CheckedRequirementKind::Copied &&
        post.kind != core::CheckedRequirementKind::Zeroed)
      continue;
    const auto first = builder.affineFromPath(post.begin, call);
    const auto last = builder.affineFromPath(post.end, call);
    const auto guard = checkedGuard(post.when, call, state);
    if (!first || !last || !guard || !guard->integers.empty() ||
        !guard->pointers.empty())
      continue;
    core::PlaceGuard frozen;
    for (const auto &[input, fact] : guard->conditions)
      frozen.require(snapshot(input), fact);
    std::vector<core::InitializedRange> ranges;
    if (post.kind == core::CheckedRequirementKind::Copied) {
      const auto source =
          checkedPathMemory(post.other, call, *first, *last, state);
      if (source)
        ranges = checkedCopyRanges(*source, *first, *last, state);
      // The relational contract preserves initialization, not byte values.
      // A callee may have overwritten initialized input with nonzero data.
      for (auto &range : ranges)
        range.zeroed = false;
    } else {
      ranges.push_back(
          {.begin = *first,
           .end = *last,
           .zeroed = post.kind == core::CheckedRequirementKind::Zeroed});
    }
    for (auto range : ranges) {
      if (range.when.size() + frozen.size() > core::MaxGuardConjuncts ||
          !range.when.integers.empty() || !range.when.pointers.empty())
        continue;
      auto combined = frozen;
      bool feasible = true;
      for (const auto &[input, fact] : range.when.conditions)
        feasible &= combined.learn(snapshot(input), fact) !=
                    core::GuardRefinement::Refuted;
      if (!feasible)
        continue;
      range.begin = freeze(range.begin);
      range.end = freeze(range.end);
      range.when = std::move(combined);
      posts.push_back({.path = post.path, .range = range, .on = post.on});
    }
  }
}

void FunctionDataflow::applyCheckedResult(core::PlaceId dest,
                                          const CallExpr &call,
                                          core::AnalysisState &state) {
  const auto found = checkedPosts.find(&call);
  if (found == checkedPosts.end())
    return;
  for (const auto &post : found->second) {
    if (!post.path.isResult() || (post.on && post.on != core::Outcome::NonNull))
      continue;
    std::optional<core::PlaceId> holder = dest;
    for (const auto &step : post.path.steps) {
      holder = places.child(*holder, step.step, step.field);
      if (!holder)
        break;
    }
    const auto memory = holder ? checkedMemoryAt(*holder, post.range.begin,
                                                 post.range.end, state)
                               : std::nullopt;
    if (!memory)
      continue;
    auto range = post.range;
    range.begin = memory->begin;
    range.end = memory->end;
    if (post.on) {
      if (range.when.size() == core::MaxGuardConjuncts)
        continue;
      range.when.require(dest, core::ValueFact::of(core::Outcome::NonNull));
    }
    state.safety->initialize(memory->storage, range);
  }
}

void FunctionDataflow::applyCheckedPosts(const CallExpr &call,
                                         const core::FunctionSummary &summary,
                                         core::AnalysisState &state) {
  const auto found = checkedPosts.find(&call);
  if (found == checkedPosts.end())
    return;
  for (const auto &post : found->second) {
    if (post.path.isResult())
      continue;
    const auto memory = checkedPathMemory(post.path, call, post.range.begin,
                                          post.range.end, state);
    if (!memory)
      continue;
    auto range = post.range;
    range.begin = memory->begin;
    range.end = memory->end;
    if (!post.on) {
      state.safety->initialize(memory->storage, range);
      continue;
    }
    if (summary.outcomes.empty())
      continue;
    if (!lastCall || lastCall->call != &call) {
      core::PendingOutcome outcome;
      outcome.callee = calleeName(call);
      outcome.location = locate(call);
      lastCall = CallOutcome{.call = &call, .pending = std::move(outcome)};
    }
    for (const auto &[outcome, effects] : summary.outcomes) {
      (void)effects;
      lastCall->pending.consumedBy.try_emplace(outcome);
    }
    lastCall->pending.initializedOn[*post.on].emplace_back(memory->storage,
                                                           range);
  }
}

} // namespace weavec::analysis
