//===- DataflowSpans.cpp - Entry byte spans (RFC 0029) --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

#include <algorithm>

using namespace clang;
namespace weavec::analysis {

static const VarDecl *spanVariable(const Expr *expression) {
  const auto *reference =
      dyn_cast<DeclRefExpr>(expression->IgnoreParenImpCasts());
  return reference ? dyn_cast<VarDecl>(reference->getDecl()) : nullptr;
}

static const ParmVarDecl *
spanParameter(const Expr *expression, const std::set<const VarDecl *> &changed,
              unsigned depth = 0) {
  const auto *variable = spanVariable(expression);
  if (!variable || depth == 12 || changed.contains(variable) ||
      variable->getType().isVolatileQualified() ||
      variable->getType()->isAtomicType())
    return nullptr;
  if (const auto *parameter = dyn_cast<ParmVarDecl>(variable))
    return parameter;
  return variable->hasLocalStorage() && variable->getInit()
             ? spanParameter(variable->getInit(), changed, depth + 1)
             : nullptr;
}

void FunctionDataflow::initializeCheckedSpans(core::AnalysisState &state) {
  const auto bytePointer = [](QualType type) {
    return type->isPointerType() && type->getPointeeType()->isCharType() &&
           !type.isVolatileQualified() &&
           !type->getPointeeType().isVolatileQualified();
  };
  unsigned endpoints = 0;
  for (const auto *parameter : function.parameters())
    endpoints += bytePointer(parameter->getType()) ? 1U : 0U;
  if (endpoints == 0)
    return;
  std::vector<const Stmt *> syntax{function.getBody()};
  std::set<const VarDecl *> changed;
  std::vector<const BinaryOperator *> differences;
  std::vector<const BinaryOperator *> comparisons;
  for (std::size_t i = 0; i < syntax.size(); ++i) {
    const auto *statement = syntax[i];
    if (const auto *unary = dyn_cast<UnaryOperator>(statement);
        unary &&
        (unary->isIncrementDecrementOp() || unary->getOpcode() == UO_AddrOf))
      if (const auto *variable = spanVariable(unary->getSubExpr()))
        changed.insert(variable);
    if (const auto *binary = dyn_cast<BinaryOperator>(statement)) {
      if (binary->getOpcode() == BO_LT)
        comparisons.push_back(binary);
      if (binary->isAssignmentOp())
        if (const auto *variable = spanVariable(binary->getLHS()))
          changed.insert(variable);
      const auto left = binary->getLHS()->getType();
      const auto right = binary->getRHS()->getType();
      if (binary->getOpcode() == BO_Sub && left->isPointerType() &&
          right->isPointerType() && left->getPointeeType()->isCharType() &&
          !left->getPointeeType().isVolatileQualified() &&
          !right->getPointeeType().isVolatileQualified() &&
          ASTContext::hasSameUnqualifiedType(left->getPointeeType(),
                                             right->getPointeeType()))
        differences.push_back(binary);
    }
    if (const auto *trait = dyn_cast<UnaryExprOrTypeTraitExpr>(statement);
        trait &&
        (trait->isArgumentType() ||
         !trait->getArgumentExpr()->getType()->isVariablyModifiedType()))
      continue;
    for (const auto *child : statement->children()) {
      if (!child)
        continue;
      if (syntax.size() == 65536)
        return;
      syntax.push_back(child);
    }
  }
  using Pair = std::pair<const ParmVarDecl *, const ParmVarDecl *>;
  std::set<Pair> candidates;
  std::map<const ParmVarDecl *, unsigned> uses;
  std::set<Pair> intervals;
  std::map<const ParmVarDecl *, unsigned> intervalUses;
  for (const auto *comparison : comparisons) {
    const auto *difference =
        dyn_cast<BinaryOperator>(comparison->getLHS()->IgnoreParenCasts());
    if (!difference || difference->getOpcode() != BO_Sub ||
        !bytePointer(difference->getLHS()->getType()) ||
        !bytePointer(difference->getRHS()->getType()) ||
        !ASTContext::hasSameUnqualifiedType(
            difference->getLHS()->getType()->getPointeeType(),
            difference->getRHS()->getType()->getPointeeType()))
      continue;
    const auto *base = spanParameter(difference->getRHS(), changed);
    const auto *count = spanParameter(comparison->getRHS(), changed);
    if (!base || !count || base == count ||
        base->getDeclContext() != &function ||
        count->getDeclContext() != &function ||
        !count->getType()->isUnsignedIntegerType() ||
        count->getType()->isBooleanType())
      continue;
    if (intervals.emplace(base, count).second) {
      ++intervalUses[base];
      ++intervalUses[count];
    }
    if (intervals.size() > 16)
      return;
  }
  for (const auto *difference : differences) {
    const auto *first = spanParameter(difference->getRHS(), changed);
    const auto *last = spanParameter(difference->getLHS(), changed);
    if (!first || !last || first == last || !bytePointer(first->getType()) ||
        !bytePointer(last->getType()) || first->getDeclContext() != &function ||
        last->getDeclContext() != &function)
      continue;
    if (candidates.emplace(first, last).second) {
      ++uses[first];
      ++uses[last];
    }
    if (candidates.size() > 16)
      return;
  }
  const auto type = integerTypeOf(context.getPointerDiffType(), context);
  const auto maximum =
      type ? core::IntegerRange::full(*type).maximum() : std::nullopt;
  const auto limit = maximum ? maximum->signedValue() : std::nullopt;
  if (!type || !limit || *limit <= 0)
    return;
  for (const auto &[base, count] : intervals) {
    const auto countType = integerTypeOf(*count, context);
    if (!countType || intervalUses[base] != 1 || intervalUses[count] != 1)
      continue;
    const auto holder = builder.placeForVar(*base);
    const auto size = builder.placeForVar(*count);
    const auto storage = places.deref(holder);
    const auto length = core::Affine::ofPlace(size);
    const auto path = core::SummaryPath::param(base->getFunctionScopeIndex());
    const auto countPath =
        core::SummaryPath::param(count->getFunctionScopeIndex());
    const auto largest = core::IntegerRange::full(*countType).maximum()->bits;
    const auto bound = std::min(largest, static_cast<std::uint64_t>(*limit));
    const auto range = core::IntegerRange::between(
        core::IntegerValue::ofBits(*countType, 0),
        core::IntegerValue::ofBits(*countType, bound));
    state.scalars.set(
        size, core::ValueFact::ofInteger(
                  integerRangeAt(size, *countType, state).intersect(range)));
    state.relations.learnAtLeast(size, 0);
    state.relations.learnAtMost(size, static_cast<std::int64_t>(bound));
    checkedInputObjects[holder] = storage;
    state.safety->pointers.insert(holder);
    state.nulls.set(holder, {.state = core::Nullness::NonNull,
                             .location = {},
                             .reason = core::NullReason::Declared});
    state.safety->accessible[storage] = length;
    state.safety->positions[holder] = {
        .storage = storage, .offset = {}, .extent = length, .input = holder};
    state.safety->initialize(storage, {.begin = {}, .end = length});
    inferred.checked.require({.kind = core::CheckedRequirementKind::Valid,
                              .path = path,
                              .other = {},
                              .family = {}});
    for (const auto kind : {core::CheckedRequirementKind::Extent,
                            core::CheckedRequirementKind::Initialized})
      inferred.checked.require({.kind = kind,
                                .path = path,
                                .other = {},
                                .end = core::PathAffine::ofPath(countPath),
                                .family = {}});
    if (largest > bound) {
      const auto slack = core::IntegerExpression<core::SummaryPath>::constant(
          core::IntegerValue::ofBits(*countType, largest - bound));
      inferred.checked.require({.kind = core::CheckedRequirementKind::SumFits,
                                .path = {},
                                .other = {},
                                .begin = core::PathAffine::ofPath(countPath),
                                .end = core::PathAffine::ofExpression(slack),
                                .family = {}});
    }
  }
  for (const auto &[first, last] : candidates) {
    if (uses[first] != 1 || uses[last] != 1)
      continue;
    const auto begin = builder.placeForVar(*first);
    const auto end = builder.placeForVar(*last);
    // This sufficient entry condition is installed only in initialState.
    // Flow-sensitive invalidation cannot recreate it at a later subtraction.
    const auto distance =
        places.create("span(" + nameOf(begin) + "," + nameOf(end) + ")");
    checkedSpans[{begin, end}] = distance;
    const auto length = core::Affine::ofPlace(distance);
    const auto storage = places.deref(begin);
    state.scalars.set(distance,
                      core::ValueFact::ofInteger(core::IntegerRange::between(
                          core::IntegerValue::ofBits(*type, 0), *maximum)));
    state.relations.learnAtLeast(distance, 0);
    state.relations.learnAtMost(distance, *limit);
    state.safety->accessible[storage] = length;
    state.safety->initialize(
        storage, {.begin = {}, .end = length, .when = {}, .source = {}});
    for (const auto holder : {begin, end}) {
      checkedInputObjects[holder] = storage;
      state.safety->pointers.insert(holder);
      state.nulls.set(holder, {.state = core::Nullness::NonNull,
                               .location = {},
                               .reason = core::NullReason::Declared});
      state.safety->positions[holder] = {
          .storage = storage,
          .offset = holder == begin ? core::Affine::ofConstant(0) : length,
          .extent = length};
    }
    inferred.checked.require(
        {.kind = core::CheckedRequirementKind::InitializedSpan,
         .path = core::SummaryPath::param(first->getFunctionScopeIndex()),
         .other = core::SummaryPath::param(last->getFunctionScopeIndex()),
         .end = core::PathAffine::ofConstant(*limit),
         .family = {}});
  }
}

bool FunctionDataflow::checkedSpanCall(
    const core::CheckedRequirement &requirement, const CallExpr &call,
    core::AnalysisState &state) {
  const auto first = checkedPathMemory(requirement.path, call, {}, {}, state);
  const auto last = checkedPathMemory(requirement.other, call, {}, {}, state);
  if (!first || !last || first->storage != last->storage || !first->extent ||
      !last->extent || !checkedValid(*first, state) ||
      !checkedValid(*last, state) ||
      !checkedInterval(first->begin, first->end, *first->extent, state) ||
      !checkedInterval(last->begin, last->end, *last->extent, state) ||
      !checkedAtMost(first->begin, last->begin, state))
    return false;
  const auto maximum = first->begin.shifted(requirement.end.constant);
  // Both coordinates are already nonnegative. Bounding the final coordinate
  // also bounds their difference, without overflowing first + PTRDIFF_MAX.
  if ((!maximum || !checkedAtMost(last->begin, *maximum, state)) &&
      !checkedAtMost(last->begin,
                     core::Affine::ofConstant(requirement.end.constant), state))
    return false;
  auto interval = *first;
  interval.end = last->begin;
  return checkedInitialized(interval, state);
}

void FunctionDataflow::checkedSpanCountBounds(core::AnalysisState &state) {
  if (!state.safety || checkedSpans.empty())
    return;
  for (const auto &[endpoints, distance] : checkedSpans) {
    (void)endpoints;
    const auto lower = state.relations.atLeast(distance);
    if (!lower)
      continue;
    for (const auto counter : checkedLoopCounters) {
      const auto fact = state.scalars.factOf(counter);
      if (fact && fact->constant && *fact->constant >= 0 &&
          *fact->constant <= *lower)
        state.relations.learn(counter, core::Relation::LessEqual, distance);
    }
  }
}

void FunctionDataflow::checkedSpanOutputs(
    core::CheckedContract &outputs, const core::AnalysisState &state,
    const Expr *value, std::optional<core::Outcome> outcome) {
  if (checkedSpans.empty() || !value || !value->getType()->isIntegerType())
    return;
  auto current = state;
  const auto range = integerRangeOf(*value, current);
  const auto count = integerAffineOf(*value, current);
  if (!range || range->mayBeInvalid || !count ||
      !checkedAtMost({}, *count, current))
    return;
  for (const auto &[endpoints, distance] : checkedSpans) {
    const auto first = builder.summaryPathOf(endpoints.first);
    const auto last = builder.summaryPathOf(endpoints.second);
    if (!first || !last ||
        !checkedAtMost(*count, core::Affine::ofPlace(distance), current))
      continue;
    outputs.establish({.kind = core::CheckedRequirementKind::CountWithinSpan,
                       .path = *first,
                       .other = *last,
                       .family = {},
                       .on = outcome});
  }
}

} // namespace weavec::analysis
