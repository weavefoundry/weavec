//===- DataflowArrayCleanup.cpp - Proved contiguous cleanup loops --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "weavec/Analysis/Allocators.h"

#include "llvm/ADT/ScopeExit.h"

using namespace clang;

namespace weavec::analysis {

static const VarDecl *arrayLoopVariable(const Expr *expr) {
  const auto *ref =
      expr ? dyn_cast<DeclRefExpr>(expr->IgnoreParenImpCasts()) : nullptr;
  return ref ? dyn_cast<VarDecl>(ref->getDecl()) : nullptr;
}

void FunctionDataflow::collectArrayCleanupLoops(const Stmt *stmt) {
  if (!stmt)
    return;
  for (const auto *child : stmt->children())
    collectArrayCleanupLoops(child);
  const auto *loop = dyn_cast<ForStmt>(stmt);
  if (!loop)
    return;
  const auto *decl = dyn_cast_or_null<DeclStmt>(loop->getInit());
  const VarDecl *variable = nullptr;
  const Expr *initial = nullptr;
  if (decl && decl->isSingleDecl()) {
    variable = dyn_cast<VarDecl>(decl->getSingleDecl());
    initial = variable ? variable->getInit() : nullptr;
  } else if (const auto *assignment =
                 dyn_cast_or_null<BinaryOperator>(loop->getInit());
             assignment && assignment->getOpcode() == BO_Assign) {
    variable = arrayLoopVariable(assignment->getLHS());
    initial = assignment->getRHS();
  }
  if (!variable || !variable->hasLocalStorage() || isa<ParmVarDecl>(variable) ||
      !initial || integerConstant(*initial, context) != 0)
    return;
  const auto *condition = dyn_cast_or_null<BinaryOperator>(loop->getCond());
  const auto *increment = dyn_cast_or_null<UnaryOperator>(loop->getInc());
  if (!condition || condition->getOpcode() != BO_LT ||
      arrayLoopVariable(condition->getLHS()) != variable ||
      condition->getRHS()->HasSideEffects(context) || !increment ||
      !increment->isIncrementOp() ||
      arrayLoopVariable(increment->getSubExpr()) != variable)
    return;
  // The bound must be a literal or a stable scalar place outside the loop.
  const auto bound = builder.affineOf(*condition->getRHS());
  if (!bound || bound->place == builder.placeForVar(*variable))
    return;
  std::vector<const Stmt *> statements;
  if (const auto *body = dyn_cast<CompoundStmt>(loop->getBody()))
    statements.assign(body->body_begin(), body->body_end());
  else
    statements.push_back(loop->getBody());
  if (statements.empty() || statements.size() > 2)
    return;
  const auto ignorePlaces = [this](auto &&self, const Stmt *body) -> void {
    if (!body)
      return;
    if (const auto *expr = dyn_cast<Expr>(body))
      roles[expr] = Role::Ignore;
    for (const auto *child : body->children())
      self(self, child);
  };
  const auto dependsOnIndex = [variable](auto &&self,
                                         const Stmt *body) -> bool {
    if (!body)
      return false;
    if (const auto *ref = dyn_cast<DeclRefExpr>(body);
        ref && ref->getDecl() == variable)
      return true;
    return std::ranges::any_of(
        body->children(), [&](const Stmt *child) { return self(self, child); });
  };
  if (statements.size() == 1) {
    const auto *assignment = dyn_cast<BinaryOperator>(statements.front());
    const auto *element = assignment && assignment->getOpcode() == BO_Assign
                              ? dyn_cast<ArraySubscriptExpr>(
                                    assignment->getLHS()->IgnoreParenImpCasts())
                              : nullptr;
    if (element && element->getType()->isPointerType() &&
        arrayLoopVariable(element->getIdx()) == variable &&
        !element->getBase()->HasSideEffects(context) &&
        !dependsOnIndex(dependsOnIndex, element->getBase())) {
      const auto *value = assignment->getRHS()->IgnoreParenImpCasts();
      std::optional<std::int64_t> bytes;
      bool supported = value->isNullPointerConstant(
                           context, Expr::NPC_ValueDependentIsNotNull) != 0U;
      const auto *allocation = dyn_cast<CallExpr>(value);
      if (allocation && allocation->getDirectCallee() &&
          allocation->getDirectCallee()->getName() == "malloc" &&
          allocation->getNumArgs() == 1) {
        const auto effects = classifyCall(*allocation, summaries);
        bytes = integerConstant(*allocation->getArg(0), context);
        supported = effects && effects->source == SummarySource::Builtin &&
                    bytes && *bytes >= 0;
      }
      if (supported) {
        ignorePlaces(ignorePlaces, loop->getBody());
        arrayFillLoops.emplace(loop, ArrayFillLoop{.assignment = assignment,
                                                   .element = element,
                                                   .count = condition->getRHS(),
                                                   .bytes = bytes});
        arrayCleanupStores.insert(assignment);
        if (allocation)
          arrayCleanupCalls.insert(allocation);
        return;
      }
    }
  }
  const auto *release = dyn_cast<CallExpr>(statements.front());
  if (!release || release->getNumArgs() != 1 || !release->getDirectCallee() ||
      release->getDirectCallee()->getName() != "free")
    return;
  const auto effects = classifyCall(*release, summaries);
  if (!effects || effects->source != SummarySource::Builtin)
    return;
  const auto *element =
      dyn_cast<ArraySubscriptExpr>(release->getArg(0)->IgnoreParenImpCasts());
  if (!element || !element->getType()->isPointerType() ||
      arrayLoopVariable(element->getIdx()) != variable ||
      element->getBase()->HasSideEffects(context) ||
      dependsOnIndex(dependsOnIndex, element->getBase()))
    return;
  const BinaryOperator *clear = nullptr;
  if (statements.size() == 2) {
    clear = dyn_cast<BinaryOperator>(statements.back());
    const auto *target = clear ? dyn_cast<ArraySubscriptExpr>(
                                     clear->getLHS()->IgnoreParenImpCasts())
                               : nullptr;
    if (!clear || clear->getOpcode() != BO_Assign || !target ||
        arrayLoopVariable(target->getIdx()) != variable ||
        target->getBase()->HasSideEffects(context) ||
        !Expr::isSameComparisonOperand(element->getBase(), target->getBase()) ||
        !clear->getRHS()->isNullPointerConstant(
            context, Expr::NPC_ValueDependentIsNotNull))
      return;
    const auto a = builder.resolve(*element->getBase());
    const auto b = builder.resolve(*target->getBase());
    if (!a || !b || a->place != b->place)
      return;
  }
  arrayCleanupLoops.emplace(loop,
                            ArrayCleanupLoop{.release = release,
                                             .element = element,
                                             .count = condition->getRHS(),
                                             .cleared = clear != nullptr});
  ignorePlaces(ignorePlaces, loop->getBody());
  arrayCleanupCalls.insert(release);
  if (clear)
    arrayCleanupStores.insert(clear);
}

void FunctionDataflow::completeArrayCleanupLoop(const CFGBlock &from,
                                                unsigned succIndex,
                                                core::AnalysisState &state) {
  if (succIndex != 1)
    return;
  const auto *loop = dyn_cast_or_null<ForStmt>(from.getTerminatorStmt());
  if (const auto fill = arrayFillLoops.find(loop);
      fill != arrayFillLoops.end()) {
    const auto &operation = fill->second;
    const auto buffer = arrayBuffer(*operation.element->getBase(), state);
    const auto count = foldAffine(builder.affineOf(*operation.count), state);
    if (buffer && count && buffer->start.isConstant() &&
        buffer->start.constant == 0) {
      arrayTypes[buffer->storage] = buffer->element;
      fillArrayRange(buffer->storage, *count, operation.bytes,
                     *operation.assignment, state);
    } else {
      reportIncomplete("unsupported contiguous array fill",
                       *operation.assignment);
    }
    return;
  }
  const auto found = arrayCleanupLoops.find(loop);
  if (found == arrayCleanupLoops.end())
    return;
  const auto &cleanup = found->second;
  const auto buffer = arrayBuffer(*cleanup.element->getBase(), state);
  const auto count = foldAffine(builder.affineOf(*cleanup.count), state);
  if (!buffer || !count || !buffer->start.isConstant() ||
      buffer->start.constant != 0) {
    reportIncomplete("unsupported contiguous array cleanup", *cleanup.release);
    return;
  }
  releaseArrayRange(buffer->storage,
                    {.begin = core::ArrayIndex::constant(0), .count = *count},
                    cleanup.cleared, *cleanup.release, state);
}

void FunctionDataflow::releaseArrayRange(core::PlaceId storage,
                                         core::ArraySpan span, bool cleared,
                                         const Expr &at,
                                         core::AnalysisState &state,
                                         std::size_t ordinal) {
  if (!span.count.place && span.count.constant <= 0)
    return;
  checkArrayTraversal(storage, span.count, at, state);
  if (recording()) {
    const auto path = stableSummaryPathOf(storage);
    const auto begin = summaryAffineOf(
        span.begin.symbol
            ? core::Affine::ofPlace(core::PlaceId{*span.begin.symbol}, 1,
                                    span.begin.offset)
            : core::Affine::ofConstant(span.begin.offset));
    const auto count = summaryAffineOf(span.count);
    if (path && begin && count)
      inferred.arrayReleases.insert({.storage = *path,
                                     .begin = *begin,
                                     .count = *count,
                                     .when = summaryGuardOf(state.pathGuard()),
                                     .cleared = cleared});
  }
  auto site = arrayReleaseSites.find({&at, ordinal});
  if (site == arrayReleaseSites.end()) {
    if (arrayReleaseSites.size() >= core::MaxArrayRanges) {
      state.incompleteHeap.insert(storage);
      reportIncomplete("array release range limit reached", at);
      return;
    }
    site = arrayReleaseSites
               .emplace(std::pair{&at, ordinal}, places.create("array-release"))
               .first;
    arrayReleaseExpressions[site->second] = &at;
  }
  state.releasedArrayRanges.insert_or_assign(
      site->second, core::ReleasedArrayRange{.storage = storage,
                                             .span = span,
                                             .materialized = {},
                                             .cleared = cleared});
  for (const auto cell : places.descendants(storage)) {
    if (places.parent(cell) != storage || !places.isElement(cell))
      continue;
    const auto index = core::ArrayIndex::parse(places.fieldName(cell));
    if (index && span.contains(*index, state.scalars, state.relations) ==
                     core::ArrayRelation::Yes)
      materializeArrayFill(storage, cell, *index, at, state);
    if (index && span.contains(*index, state.scalars, state.relations) ==
                     core::ArrayRelation::Yes)
      materializeArrayRelease(storage, cell, *index, at, state);
  }
}

void FunctionDataflow::materializeArrayRelease(core::PlaceId storage,
                                               core::PlaceId cell,
                                               const core::ArrayIndex &index,
                                               const Expr &at,
                                               core::AnalysisState &state) {
  if (materializingArrayRelease)
    return;
  materializingArrayRelease = true;
  const auto reset =
      llvm::scope_exit([&] { materializingArrayRelease = false; });
  for (auto &[key, range] : state.releasedArrayRanges) {
    if (range.storage != storage || range.materialized.contains(index))
      continue;
    const auto membership =
        range.span.contains(index, state.scalars, state.relations);
    if (membership == core::ArrayRelation::No)
      continue;
    const auto site = arrayReleaseExpressions.find(key);
    if (site == arrayReleaseExpressions.end())
      continue;
    if (membership == core::ArrayRelation::Unknown) {
      state.incompleteHeap.insert(storage);
      reportIncomplete("array cleanup membership is unresolved", at);
    }
    if (range.materialized.size() >= core::MaxArrayCells) {
      state.incompleteHeap.insert(storage);
      reportIncomplete("array cleanup element limit reached", at);
      continue;
    }
    // The loop's own selected body place may already have a record from a
    // different path; doConsume preserves the earliest release evidence.
    PlaceRef ref{.place = cell,
                 .derefs = {},
                 .derefExprs = {},
                 .derefElements = {},
                 .element = {}};
    std::optional<core::AnalysisState> before;
    if (range.cleared && range.definite &&
        membership == core::ArrayRelation::Unknown)
      before = state;
    const auto previous = state.moves.recordOf(cell);
    if (!previous || previous->location != locate(*site->second) ||
        previous->reason != core::MoveReason::Freed ||
        (previous->via && previous->via != cell))
      (void)doConsume(ref, core::MoveReason::Freed, *site->second, state,
                      "free", true);
    range.materialized.insert(index);
    if (range.cleared && range.definite) {
      ValueOrigin nil;
      nil.kind = ValueOrigin::Kind::Null;
      applyPointerAssign(cell, nil, *site->second, false, state);
    }
    if (before)
      state.join(*before, &places);
  }
}

void FunctionDataflow::applyArrayReleases(const CallExpr &call,
                                          const core::FunctionSummary &summary,
                                          core::AnalysisState &state) {
  std::size_t ordinal = 0;
  for (const auto &release : summary.arrayReleases) {
    auto when = builder.translateGuard(release.when, call);
    if (!when || !pruneGuard(*when, state))
      continue;
    const auto storage =
        builder.resolveSummaryPath(release.storage, call, true);
    const auto begin =
        foldAffine(builder.affineFromPath(release.begin, call), state);
    const auto count =
        foldAffine(builder.affineFromPath(release.count, call), state);
    if (!storage || !begin || !count || (begin->place && begin->scale != 1)) {
      reportIncomplete("unresolved array cleanup at call", call);
      continue;
    }
    const auto index =
        begin->place
            ? core::ArrayIndex::variable(begin->place->value, begin->constant)
            : core::ArrayIndex::constant(begin->constant);
    releaseArrayRange(storage->place, {.begin = index, .count = *count},
                      release.cleared && release.definite && when->trivial(),
                      call, state, ordinal++);
  }
}

} // namespace weavec::analysis
