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
      if ((range.numericText || range.zeroed) &&
          checkedAtMost(range.begin, source.begin, state) &&
          checkedAtMost(source.end, range.end, state)) {
        // Source ranges use storage coordinates; copied output ranges use
        // argument-relative coordinates. Full coverage needs no subtraction
        // of two independently represented symbolic origins.
        range.begin = first;
        range.end = last;
        result.push_back(std::move(range));
        continue;
      }
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
  captureFootprintPosts(call, contract, state);
  captureBufferPosts(call, contract, state);
  auto &posts = checkedPosts[&call];
  posts.clear();
  auto &positions = checkedPositionPosts[&call];
  positions.clear();
  auto &progress = checkedProgressPosts[&call];
  progress.clear();
  auto &spanCounts = checkedSpanPosts[&call];
  spanCounts.clear();
  if (!contract.complete())
    return;
  for (const auto &post : contract.establishes) {
    if (post.kind != core::CheckedRequirementKind::AllocationConsumed ||
        post.on || !post.when.trivial() || post.family != "free" ||
        !post.path.isParam() || !post.path.isRoot() ||
        post.path.index >= call.getNumArgs())
      continue;
    const auto origin = builder.classifyValue(*call.getArg(post.path.index));
    if (!origin.place || !origin.offset.isZero())
      continue;
    const auto holder = origin.place->place;
    recordAllocationConsumed(holder, state);
    if (std::ranges::any_of(contract.establishes, [&](const auto &other) {
          return other.kind ==
                     core::CheckedRequirementKind::ContainerConsumed &&
                 other.path == post.path;
        }))
      continue;
    if (const auto *fact = state.safety->containers.find(holder)) {
      auto required = fact->shape;
      required.access = core::ContainerAccess::Release;
      required.family = "free";
      if (const auto owned = strengthenContainer(*fact, required);
          owned && requireContainer(*owned, call, state)) {
        state.safety->containers.set(holder, *owned);
        releaseFootprint(holder, false, state);
      }
    } else if (footprintHeads.contains(holder)) {
      // RFC 0029: the helper consumes this ordinary allocation exactly as a
      // modeled nullable release does; null contributes the empty footprint.
      const auto resource = state.resources.recordOf(holder);
      const auto memory = checkedMemoryAt(holder, {}, {}, state);
      auto when = resource ? resource->guard : core::PlaceGuard{};
      auto nonNull = state;
      nonNull.nulls.set(holder, {.state = core::Nullness::NonNull,
                                 .location = {},
                                 .reason = core::NullReason::Declared});
      if (resource && resource->origin == core::ResourceOrigin::Allocated &&
          resource->family == "free" && !resource->escaped &&
          !state.moves.recordOf(holder) && memory &&
          memory->begin == core::Affine::ofConstant(0) &&
          checkedValid(*memory, nonNull) && pruneGuard(when, nonNull) &&
          when.trivial())
        releaseFootprint(holder, false, state);
    }
  }
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
  // RFC 0026: an exact returned input pointer keeps its entry storage view,
  // even when the callee clears the container field before returning it.
  // Every represented return must name that same pointer; a nullable
  // alternative chosen independently of the input cannot use this rule.
  // Conditional backing validity is introduced only for registered buffers.
  // Avoid resolving unrelated pointer-returning calls for this refinement.
  if (!bufferObjects.empty() && call.getType()->isPointerType()) {
    const auto effects = classifyCall(call, summaries);
    if (effects && effects->summary && !effects->summary->returns.empty()) {
      const auto &firstReturn = *effects->summary->returns.begin();
      const bool exact =
          firstReturn.kind == core::ValueSource::Kind::Copy &&
          firstReturn.path && firstReturn.offset.isZero() &&
          firstReturn.when.trivial() &&
          std::ranges::all_of(
              effects->summary->returns, [&](const auto &alternative) {
                return alternative.kind == core::ValueSource::Kind::Copy &&
                       alternative.path == firstReturn.path &&
                       alternative.offset.isZero() &&
                       alternative.when.trivial();
              });
      const auto origin =
          exact ? checkedPathMemory(*firstReturn.path, call, {}, {}, state)
                : std::nullopt;
      if (origin && origin->validWhenNonempty) {
        bool preservesBytes = true;
        for (const auto &[path, effect] : effects->summary->effects) {
          if (!effect.written && !effect.consumed())
            continue;
          const auto target = builder.resolveSummaryPath(path, call, true);
          bool headerWrite = false;
          if (target && origin->holder)
            for (const auto &[object, shape] : bufferObjects)
              if (places.field(object, shape.data.name) == *origin->holder)
                headerWrite |=
                    target->place == places.field(object, shape.data.name) ||
                    target->place == places.field(object, shape.length.name) ||
                    target->place == places.field(object, shape.capacity.name);
          preservesBytes &= headerWrite && !effect.consumed();
        }
        positions.push_back(
            {.path = core::SummaryPath::result(),
             .position = {.storage = origin->storage,
                          .offset = freeze(origin->begin),
                          .extent = origin->extent
                                        ? std::optional(freeze(*origin->extent))
                                        : std::nullopt,
                          .input = origin->inputPlace,
                          .validWhenNonempty = true},
             .upper = {},
             .when = {},
             .on = {},
             .nonNull = checkedValid(*origin, state)});
        if (const auto initialized = state.safety->memory.find(origin->storage);
            initialized != state.safety->memory.end())
          for (auto range : initialized->second) {
            if (range.source || !range.when.trivial())
              continue;
            range.begin = freeze(range.begin);
            range.end = freeze(range.end);
            range.zeroed &= preservesBytes;
            range.numericText &= preservesBytes;
            posts.push_back({.path = core::SummaryPath::result(),
                             .range = range,
                             .on = {},
                             .storage = {},
                             .objectType = {}});
          }
      }
    }
  }
  for (const auto &post : contract.establishes) {
    if (post.kind == core::CheckedRequirementKind::BufferPreserved ||
        post.kind == core::CheckedRequirementKind::BufferAppended) {
      if (auto sequenceSnapshot = bufferSequencePosts[&call].find(post);
          sequenceSnapshot != bufferSequencePosts[&call].end())
        sequenceSnapshot->second.index = freeze(sequenceSnapshot->second.index);
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::ArgumentListConsumed) {
      if (post.path.isParam() && post.path.isRoot() &&
          post.path.index < call.getNumArgs())
        if (const auto place = runtimeListPlace(*call.getArg(post.path.index)))
          state.safety->argumentLists[*place].phase =
              core::ArgumentListPhase::Consumed;
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::Buffer) {
      if (const auto bound = endpoint(post.end))
        bufferPostBounds[&call][post] = freeze(*bound);
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
    if (post.kind == core::CheckedRequirementKind::InitializedAdvance) {
      const auto origin = checkedPathMemory(post.other, call, {}, {}, state);
      if (origin && !post.path.isResult() && post.when.trivial())
        posts.push_back({.path = post.path,
                         .range = {.begin = freeze(origin->begin)},
                         .on = post.on,
                         .storage = origin->storage,
                         .objectType = {},
                         .throughPosition = true});
      continue;
    }
    if (post.kind == core::CheckedRequirementKind::CountWithinSpan) {
      const auto result = numericCallResult(call);
      const auto type = integerTypeOf(call.getType(), context);
      const auto a = checkedPathMemory(post.path, call, {}, {}, state);
      const auto b = checkedPathMemory(post.other, call, {}, {}, state);
      if (!result || !type || !a || !b || a->storage != b->storage ||
          !checkedAtMost({}, a->begin, state) ||
          !checkedAtMost(a->begin, b->begin, state))
        continue;
      const auto capture = [&](const core::Affine &coordinate) {
        auto expression = checkedByteExpression(coordinate, state);
        if (coordinate.place && coordinate.scale == 1 &&
            coordinate.constant == 0)
          if (const auto *decl = dyn_cast_or_null<ValueDecl>(
                  builder.declFor(*coordinate.place));
              decl && !decl->getType().isVolatileQualified() &&
              !decl->getType()->isAtomicType())
            if (const auto cellType = integerTypeOf(*decl, context);
                cellType && cellType->width <= 64)
              if (const auto minimum =
                      integerRangeAt(*coordinate.place, *cellType, state)
                          .minimum();
                  minimum && !minimum->negative())
                expression =
                    NumericExpression::input(*coordinate.place, *cellType)
                        .converted({.width = 64, .isSigned = false});
        return expression
                   ? expression->substitute<core::PlaceId>(
                         [&](core::PlaceId key, core::IntegerType keyType) {
                           return std::optional(NumericExpression::input(
                               snapshot(key), keyType));
                         })
                   : std::nullopt;
      };
      const auto first = capture(a->begin);
      const auto last = capture(b->begin);
      const core::IntegerType bytes{.width = 64, .isSigned = false};
      const auto count =
          NumericExpression::input(*result, *type).converted(bytes);
      const auto length = first && last
                              ? NumericExpression::operation(
                                    core::IntegerOp::Subtract, *last, *first)
                              : std::nullopt;
      if (count && length)
        spanCounts.emplace_back(*count, *length);
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
          .input = origin->inputPlace,
          .validWhenNonempty = origin->validWhenNonempty};
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
      // Only a recognized byte-copy primitive also preserves contents.
      // A general initialized-output contract may have overwritten the input.
      auto name = resolvedLibraryName(call);
      if (const auto *callee = call.getDirectCallee();
          callee && callee->getBuiltinID()) {
        if (name.starts_with("__builtin___") && name.ends_with("_chk"))
          name = name.substr(12, name.size() - 16);
        else if (name.starts_with("__builtin_"))
          name = name.substr(10);
      }
      const bool byteCopy = name == "memcpy" || name == "memmove";
      for (auto &range : ranges) {
        range.zeroed &= byteCopy;
        range.numericText &= byteCopy;
      }
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
                          state)) {
        // Position outputs are simultaneous guarantees. A wider envelope
        // must not replace an already established exact (or narrower) value.
        const auto upper = post.upper.value_or(post.position.offset);
        const bool sameExtent =
            post.position.extent &&
            checkedAtMost(*existing->extent, *post.position.extent, state) &&
            checkedAtMost(*post.position.extent, *existing->extent, state);
        if (!post.position.extent ||
            !checkedInterval(post.position.offset, upper, *post.position.extent,
                             state) ||
            (sameExtent &&
             checkedAtMost(post.position.offset, existing->begin, state) &&
             checkedAtMost(existing->end, upper, state)))
          continue;
      }
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
    } else {
      auto spatial =
          state.spatial.recordOf(*dest).value_or(core::SpatialRecord{});
      spatial.extent = post.position.extent;
      spatial.offset = core::PointerOffset::unknown();
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
    for (const auto &post : found->second) {
      if (!post.on || !post.when.trivial() ||
          !post.position.offset.isConstant() || !post.upper ||
          !post.upper->isConstant() || post.position.offset.constant < 0 ||
          post.position.offset.constant > post.upper->constant)
        continue;
      const auto dest = builder.resolveSummaryPath(post.path, call);
      if (!dest || !installed.contains(dest->place))
        continue;
      const auto actual = state.safety->positions.find(dest->place);
      const auto coordinate = checkedCoordinates.find(dest->place);
      if (actual == state.safety->positions.end() ||
          coordinate == checkedCoordinates.end() ||
          actual->second.storage != post.position.storage ||
          actual->second.offset != core::Affine::ofPlace(coordinate->second))
        continue;
      if (!lastCall || lastCall->call != &call) {
        core::PendingOutcome outcome;
        outcome.callee = calleeName(call);
        outcome.location = locate(call);
        if (call.getType()->isPointerType()) {
          outcome.consumedBy.try_emplace(core::Outcome::Null);
          outcome.consumedBy.try_emplace(core::Outcome::NonNull);
        } else if (call.getType()->isIntegerType()) {
          outcome.consumedBy.try_emplace(core::Outcome::Zero);
          outcome.consumedBy.try_emplace(core::Outcome::Positive);
          if (call.getType()->isSignedIntegerType())
            outcome.consumedBy.try_emplace(core::Outcome::Negative);
        } else {
          continue;
        }
        lastCall = CallOutcome{.call = &call, .pending = std::move(outcome)};
      }
      const core::IntegerType bytes{.width = 64, .isSigned = false};
      const auto range = core::IntegerRange::between(
          core::IntegerValue::ofBits(
              bytes, static_cast<std::uint64_t>(post.position.offset.constant)),
          core::IntegerValue::ofBits(
              bytes, static_cast<std::uint64_t>(post.upper->constant)));
      lastCall->pending.factOn[*post.on].emplace_back(
          coordinate->second, core::ValueFact::ofInteger(range));
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
  applyBufferPosts(call, state, dest);
  applyContainerPosts(call, state, dest);
  applyFootprintPosts(call, state, dest);
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
  if (const auto counts = checkedSpanPosts.find(&call);
      counts != checkedSpanPosts.end())
    for (const auto &[count, length] : counts->second) {
      if (const auto result = numericCallResult(call)) {
        state.relations.learnAtLeast(*result, 0);
        if (const auto bound = linearIntegerExpression(length, state)) {
          if (bound->isConstant())
            state.relations.learnAtMost(*result, bound->constant);
          else if (bound->place && bound->scale == 1)
            state.relations.learn(*result, core::Relation::LessEqual,
                                  *bound->place, bound->constant);
        }
      }
      if (!state.numericConditions.requireInteger(
              {.lhs = count,
               .op = core::IntegerOp::LessEqual,
               .rhs = length}) &&
          state.numericConditions.size() >= core::MaxGuardConjuncts)
        state.numericConditionsIncomplete = true;
    }
  applyCheckedPositions(call, state);
  applyContainerPosts(call, state);
  applyFootprintPosts(call, state, std::nullopt);
  applyBufferPosts(call, state);
  // RFC 0028: an accessor used directly in an expression still has a pointer
  // value. Retain its verified storage position independently of assignment.
  // This gives nested dereferences the same lifetime and bounds evidence as
  // a saved accessor result.
  if (call.getType()->isPointerType())
    if (const auto positions = checkedPositionPosts.find(&call);
        positions != checkedPositionPosts.end() &&
        std::ranges::any_of(positions->second, [](const auto &post) {
          return post.path.isResult() && post.path.isRoot();
        })) {
      const auto [entry, inserted] = checkedPointerResults.try_emplace(&call);
      if (inserted)
        entry->second =
            places.create("pointer-call@" + std::to_string(locate(call).line));
      reinit(entry->second, state);
      applyCheckedResult(entry->second, call, state);
    }
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
    if (post.throughPosition) {
      const auto output = checkedPathMemory(post.path, call, {}, {}, state);
      if (!storage || !output || output->storage != *storage ||
          !checkedAtMost(range.begin, output->begin, state) ||
          !checkedValid(*output, state) || !output->extent ||
          !checkedInterval(range.begin, output->begin, *output->extent, state))
        continue;
      range.end = output->begin;
    }
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
