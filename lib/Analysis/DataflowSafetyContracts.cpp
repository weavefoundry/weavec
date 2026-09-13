//===- DataflowSafetyContracts.cpp - Checked postconditions (RFC 0019)
//-----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"

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
  captureContainerPosts(call, contract, state);
  auto &posts = checkedPosts[&call];
  posts.clear();
  auto &positions = checkedPositionPosts[&call];
  positions.clear();
  auto &progress = checkedProgressPosts[&call];
  progress.clear();
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
    for (const auto &[pair, edge] : state.relations.allBounds()) {
      if (pair.first == input && pair.second != saved)
        state.relations.learn(saved, edge.relation, pair.second, edge.offset);
      else if (pair.second == input && pair.first != saved)
        state.relations.learn(pair.first, edge.relation, saved, edge.offset);
    }
    if (const auto lower = state.relations.atLeast(input))
      state.relations.learnAtLeast(saved, *lower);
    if (const auto upper = state.relations.atMost(input))
      state.relations.learnAtMost(saved, *upper);
    state.relations.learn(saved, core::Relation::Equal, input);
    if (const auto expression = numericExpressions.find(input);
        expression != numericExpressions.end())
      if (const auto projected = summaryIntegerExpression(expression->second))
        numericSnapshotExpressions.emplace(saved, *projected);
    if (const auto path = stableSummaryPathOf(input))
      snapshotInputPaths[saved] = *path;
    if (const auto witness = checkedTerminatorInputs.find(input);
        witness != checkedTerminatorInputs.end())
      checkedTerminatorInputs[saved] = witness->second;
    return saved;
  };
  const auto freeze = [&](core::Affine value) {
    value = foldAffine(value, state);
    if (value.place && value.place != numericCallResult(call) &&
        !checkedTerminatorInputs.contains(*value.place))
      value.place = snapshot(*value.place);
    return value;
  };
  const auto endpoint =
      [&](const core::PathAffine &value) -> std::optional<core::Affine> {
    if (value.path && value.path->isResult()) {
      const auto result = numericCallResult(call);
      if (!value.path->isRoot() || !result)
        return std::nullopt;
      return core::Affine::ofPlace(*result, value.scale, value.constant);
    }
    return builder.affineFromPath(value, call);
  };
  for (const auto &post : contract.establishes) {
    if (post.kind == core::CheckedRequirementKind::ArgumentListConsumed) {
      if (post.path.isParam() && post.path.isRoot() &&
          post.path.index < call.getNumArgs())
        if (const auto place = runtimeListPlace(*call.getArg(post.path.index)))
          state.safety->argumentLists[*place].phase =
              core::ArgumentListPhase::Consumed;
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::Container)
      continue;
    if (post.kind == core::CheckedRequirementKind::UnionMember) {
      const auto guard = checkedGuard(post.when, call, state);
      if (!guard || !guard->integers.empty() || !guard->pointers.empty())
        continue;
      core::PlaceGuard frozen;
      for (const auto &[input, fact] : guard->conditions)
        frozen.require(snapshot(input), fact);
      posts.push_back({.path = post.path,
                       .range = {.when = frozen},
                       .on = post.on,
                       .storage = {},
                       .objectType = {},
                       .unionMember = post.family});
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::ObjectType) {
      const auto guard = checkedGuard(post.when, call, state);
      if (guard && guard->trivial())
        posts.push_back({.path = post.path,
                         .range = {},
                         .on = post.on,
                         .storage = {},
                         .objectType = post.family});
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::Progress) {
      const auto guard = checkedGuard(post.when, call, state);
      const auto a = checkedPathMemory(post.path, call, {}, {}, state);
      const auto b = checkedPathMemory(post.other, call, {}, {}, state);
      if (!guard || !guard->trivial() || !post.end.isConstant() || !a || !b ||
          a->storage != b->storage || !checkedValid(*a, state) ||
          !checkedValid(*b, state) || !a->extent || !b->extent ||
          !checkedInterval(a->begin, a->end, *a->extent, state) ||
          !checkedInterval(b->begin, b->end, *b->extent, state) ||
          !checkedAtMost(a->begin, b->begin, state))
        continue;
      progress.push_back({.path = post.path,
                          .other = post.other,
                          .storage = a->storage,
                          .offset = post.end.constant,
                          .on = post.on});
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::Position) {
      const auto first = builder.affineFromPath(post.begin, call);
      const auto last = builder.affineFromPath(post.end, call);
      const auto guard = checkedGuard(post.when, call, state);
      if (!first || !last || !guard || !guard->trivial())
        continue;
      const auto origin =
          checkedPathMemory(post.other, call, *first, *last, state);
      if (!origin) {
        continue;
      }
      auto upper = origin->end;
      if (upper.place && upper.scale > 0)
        if (const auto expression = numericExpressions.find(*upper.place);
            expression != numericExpressions.end())
          if (const auto bound =
                  checkedTraversalSum(expression->second, state)) {
            const auto scaled = bound->times(upper.scale);
            if (const auto shifted =
                    scaled ? scaled->shifted(upper.constant) : std::nullopt)
              upper = *shifted;
          }
      core::PointerPosition position{
          .storage = origin->storage,
          .offset = freeze(origin->begin),
          .extent = origin->extent ? std::optional(freeze(*origin->extent))
                                   : std::nullopt,
          .input = origin->inputPlace};
      positions.push_back({.path = post.path,
                           .position = position,
                           .upper = freeze(upper),
                           .when = {},
                           .on = post.on,
                           .nonNull = checkedValid(*origin, state)});
      continue;
    }
    if (post.kind != core::CheckedRequirementKind::Initialized &&
        post.kind != core::CheckedRequirementKind::Copied &&
        post.kind != core::CheckedRequirementKind::Zeroed &&
        post.kind != core::CheckedRequirementKind::Terminated &&
        post.kind != core::CheckedRequirementKind::TerminatedWithin)
      continue;
    const auto first = endpoint(post.begin);
    const auto last = endpoint(post.end);
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
    } else if (post.kind == core::CheckedRequirementKind::Terminated) {
      const auto through = last->shifted(1);
      if (!through)
        continue;
      ranges.push_back({.begin = *first, .end = *through});
      ranges.push_back({.begin = *last, .end = *through, .zeroed = true});
    } else {
      ranges.push_back(
          {.begin = *first,
           .end = *last,
           .zeroed = post.kind == core::CheckedRequirementKind::Zeroed,
           .terminatedWithin =
               post.kind == core::CheckedRequirementKind::TerminatedWithin});
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
      std::optional<core::PlaceId> storage;
      if (!post.path.isResult()) {
        std::optional<core::SummaryPath> origin;
        bool conflictingOrigin = false;
        if (post.path.isParam() && post.path.isRoot())
          origin = post.path;
        for (const auto &position : contract.establishes)
          if (position.kind == core::CheckedRequirementKind::Position &&
              position.path == post.path &&
              (!position.on || position.on == post.on) &&
              position.when.trivial()) {
            if (origin && origin != position.other) {
              conflictingOrigin = true;
              break;
            }
            origin = position.other;
          }
        if (conflictingOrigin)
          continue;
        if (origin) {
          const auto memory =
              checkedPathMemory(*origin, call, range.begin, range.end, state);
          if (!memory)
            continue;
          storage = memory->storage;
          range.begin = memory->begin;
          range.end = memory->end;
        }
      }
      range.begin = freeze(range.begin);
      range.end = freeze(range.end);
      range.when = std::move(combined);
      posts.push_back({.path = post.path,
                       .range = range,
                       .on = post.on,
                       .storage = storage,
                       .ifNonNull = post.ifNonNull,
                       .objectType = {}});
    }
  }
}

void FunctionDataflow::applyCheckedPositions(
    const CallExpr &call, core::AnalysisState &state,
    std::optional<core::PlaceId> result) {
  const auto found = checkedPositionPosts.find(&call);
  if (found == checkedPositionPosts.end())
    return;
  std::set<core::PlaceId> installed;
  for (const auto &post : found->second) {
    if (post.path.isResult() != result.has_value())
      continue;
    if (post.on && !(result && post.on == core::Outcome::NonNull)) {
      const auto fact = scalarFactOf(call, state);
      if (!fact || !fact->implies(core::ValueFact::of(*post.on)))
        continue;
    }
    auto when = post.when;
    if (!pruneGuard(when, state) || !when.trivial())
      continue;
    std::optional<core::PlaceId> dest = result;
    if (result) {
      for (const auto &step : post.path.steps) {
        dest = places.child(*dest, step.step, step.field);
        if (!dest)
          break;
      }
    } else if (const auto ref = builder.resolveSummaryPath(post.path, call)) {
      dest = ref->place;
    }
    if (!dest)
      continue;
    if (installed.contains(*dest))
      if (const auto existing = checkedMemoryAt(*dest, {}, {}, state);
          existing && existing->storage == post.position.storage &&
          existing->extent &&
          checkedInterval(existing->begin, existing->end, *existing->extent,
                          state) &&
          (!post.position.extent ||
           !checkedInterval(post.position.offset,
                            post.upper.value_or(post.position.offset),
                            *post.position.extent, state)))
        continue;
    auto position = post.position;
    if (post.upper &&
        foldAffine(*post.upper, state) != foldAffine(position.offset, state)) {
      const auto first = foldAffine(position.offset, state);
      const auto last = foldAffine(*post.upper, state);
      if (!checkedAtMost(first, last, state) ||
          !checkedAtMost({}, first, state))
        continue;
      auto [slot, inserted] = checkedCoordinates.try_emplace(*dest);
      if (inserted)
        slot->second = places.create("position(" + nameOf(*dest) + ")");
      const auto coordinate = slot->second;
      snapshotScalar(coordinate, &call, state);
      state.dropGuardsOn(coordinate);
      state.relations.forget(coordinate);
      state.scalars.forget(coordinate);
      const auto bound = [&](const core::Affine &value, bool lower) {
        if (!value.place) {
          if (lower)
            state.relations.learnAtLeast(coordinate, value.constant);
          else
            state.relations.learnAtMost(coordinate, value.constant);
        } else if (value.scale == 1) {
          state.relations.learn(coordinate,
                                lower ? core::Relation::GreaterEqual
                                      : core::Relation::LessEqual,
                                *value.place, value.constant);
        }
      };
      bound(first, true);
      bound(last, false);
      position.offset = core::Affine::ofPlace(coordinate);
    }
    installCheckedPosition(*dest, position, state);
    installed.insert(*dest);
    state.safety->objects[*dest] = post.position.storage;
    state.safety->initialized.insert(*dest);
    // Keep ordinary reachable-boundary diagnostics consistent with the
    // proved cursor value instead of retaining the overwritten offset.
    const auto offset = foldAffine(position.offset, state);
    const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(*dest));
    const auto unit =
        decl && decl->getType()->isPointerType()
            ? byteSizeOf(decl->getType()->getPointeeType(), context)
            : std::nullopt;
    if (unit && *unit > 0 && offset.isConstant() &&
        offset.constant % *unit == 0) {
      auto spatial =
          state.spatial.recordOf(*dest).value_or(core::SpatialRecord{});
      spatial.extent = post.position.extent;
      spatial.offset = core::PointerOffset::ofElements(offset.constant / *unit);
      spatial.boundsOffset = spatial.offset;
      state.spatial.set(*dest, spatial);
    }
    if (post.nonNull && (!post.on || !result)) {
      state.safety->pointers.insert(*dest);
      state.nulls.set(*dest, {.state = core::Nullness::NonNull,
                              .location = {},
                              .reason = core::NullReason::Declared});
    }
  }
  if (!result)
    if (const auto progress = checkedProgressPosts.find(&call);
        progress != checkedProgressPosts.end())
      for (const auto &post : progress->second) {
        if (post.on) {
          const auto fact = scalarFactOf(call, state);
          if (!fact || !fact->implies(core::ValueFact::of(*post.on)))
            continue;
        }
        const auto a = checkedPathMemory(post.path, call, {}, {}, state);
        const auto b = checkedPathMemory(post.other, call, {}, {}, state);
        if (!a || !b || a->storage != post.storage ||
            b->storage != post.storage || !a->begin.place || !b->begin.place ||
            a->begin.scale != 1 || b->begin.scale != 1 ||
            !checkedValid(*a, state) || !checkedValid(*b, state))
          continue;
        std::int64_t offset = 0;
        if (!__builtin_sub_overflow(b->begin.constant, a->begin.constant,
                                    &offset) &&
            !__builtin_add_overflow(offset, post.offset, &offset))
          state.relations.learn(*a->begin.place, core::Relation::LessEqual,
                                *b->begin.place, offset);
      }
}

void FunctionDataflow::applyCheckedResult(core::PlaceId dest,
                                          const CallExpr &call,
                                          core::AnalysisState &state) {
  applyCheckedPositions(call, state, dest);
  applyContainerPosts(call, state, dest);
  applyCheckedUnionPosts(call, state, dest);
  const auto found = checkedPosts.find(&call);
  if (found == checkedPosts.end())
    return;
  for (const auto &post : found->second) {
    if (!post.unionMember.empty())
      continue;
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
    if (!post.objectType.empty()) {
      if (state.safety->objectTypes.size() < core::MaxSafetyRequirements ||
          state.safety->objectTypes.contains(memory->storage))
        state.safety->objectTypes[memory->storage] = post.objectType;
      else if (recording())
        inferred.checked.limited = true;
      continue;
    }
    auto range = post.range;
    range.begin = memory->begin;
    range.end = memory->end;
    if (post.ifNonNull) {
      if (range.when.size() == core::MaxGuardConjuncts)
        continue;
      range.when.require(*holder, core::ValueFact::of(core::Outcome::NonNull));
    }
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
  applyCheckedPositions(call, state);
  applyContainerPosts(call, state);
  applyCheckedUnionPosts(call, state);
  const auto found = checkedPosts.find(&call);
  if (found == checkedPosts.end())
    return;
  for (const auto &post : found->second) {
    if (!post.unionMember.empty())
      continue;
    if (post.path.isResult())
      continue;
    if (!post.objectType.empty()) {
      const auto output = checkedPathMemory(post.path, call, {}, {}, state);
      const auto outcome = scalarFactOf(call, state);
      if (output &&
          (!post.on ||
           (outcome && outcome->implies(core::ValueFact::of(*post.on))))) {
        if (state.safety->objectTypes.size() < core::MaxSafetyRequirements ||
            state.safety->objectTypes.contains(output->storage))
          state.safety->objectTypes[output->storage] = post.objectType;
        else if (recording())
          inferred.checked.limited = true;
      }
      continue;
    }
    auto storage = post.storage;
    auto range = post.range;
    if (!storage) {
      const auto memory =
          checkedPathMemory(post.path, call, range.begin, range.end, state);
      if (!memory)
        continue;
      storage = memory->storage;
      range.begin = memory->begin;
      range.end = memory->end;
    }
    if (post.ifNonNull) {
      const auto output = builder.resolveSummaryPath(post.path, call);
      if (!output || range.when.size() == core::MaxGuardConjuncts)
        continue;
      range.when.require(output->place,
                         core::ValueFact::of(core::Outcome::NonNull));
    }
    if (!post.on) {
      state.safety->initialize(*storage, range);
      continue;
    }
    if (const auto result = numericCallResult(call)) {
      if (range.when.size() < core::MaxGuardConjuncts) {
        const auto type = integerTypeOf(call.getType(), context);
        if (!type)
          continue;
        range.when.require(*result,
                           core::ValueFact::ofInteger(
                               core::ValueFact::of(*post.on).inType(*type)));
        state.safety->initialize(*storage, range);
      }
      continue;
    }
    if (summary.outcomes.empty() && !call.getType()->isPointerType())
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
    if (summary.outcomes.empty()) {
      lastCall->pending.consumedBy.try_emplace(core::Outcome::Null);
      lastCall->pending.consumedBy.try_emplace(core::Outcome::NonNull);
    }
    lastCall->pending.initializedOn[*post.on].emplace_back(*storage, range);
  }
  // Reconstruct an entry witness when the callee proves that very byte is
  // still zero and the entire prefix remains initialized in the call frame.
  // Its immutable index needs no per-call snapshot or changed join identity.
  for (const auto &post : found->second) {
    if (!post.unionMember.empty())
      continue;
    if (!post.storage || post.on || !post.range.zeroed ||
        !post.range.when.trivial() || !post.range.begin.place ||
        post.range.begin.scale != 1 || post.range.begin.constant != 0)
      continue;
    const auto input = checkedTerminatorInputs.find(*post.range.begin.place);
    const auto through = post.range.begin.shifted(1);
    if (input == checkedTerminatorInputs.end() || !through ||
        *through != post.range.end)
      continue;
    CheckedMemory prefix{.storage = *post.storage,
                         .begin = {},
                         .end = *through,
                         .extent = through,
                         .input = {},
                         .pointer = nullptr};
    if (!checkedInitialized(prefix, state))
      continue;
    auto &witnesses = state.safety->termination[*post.storage];
    const core::TerminationWitness witness{.begin = {},
                                           .zero = post.range.begin,
                                           .input = input->second,
                                           .when = {}};
    if (std::ranges::find(witnesses, witness) == witnesses.end() &&
        witnesses.size() < core::MaxInitializedRanges)
      witnesses.push_back(witness);
  }
}

} // namespace weavec::analysis
