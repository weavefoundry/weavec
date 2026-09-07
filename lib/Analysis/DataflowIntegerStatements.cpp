//===- DataflowIntegerStatements.cpp - Numeric writes and edges (RFC 0017) ===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

using namespace clang;

namespace weavec::analysis {

void FunctionDataflow::handleIntegerCompound(const CompoundAssignOperator &expr,
                                             core::AnalysisState &state) {
  const auto ref = builder.resolve(*expr.getLHS());
  if (!ref || !expr.getType()->isIntegerType())
    return;
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(ref->place));
  auto storage = decl ? integerTypeOf(*decl, context) : std::nullopt;
  if (!storage)
    storage = integerTypeOf(expr.getType(), context);
  const auto computation = integerTypeOf(expr.getComputationLHSType(), context);
  const auto rhs = integerRangeOf(*expr.getRHS(), state);
  const auto op = integerOpOf(expr.getOpcode());
  if (!storage || !computation || !rhs || !op) {
    forgetScalar(ref->place, state, &expr);
    reportIncomplete("unsupported compound integer assignment", expr);
    return;
  }
  const auto old =
      integerRangeAt(ref->place, *storage, state).converted(*computation);
  const auto result = core::evaluateInteger(
      *op, old, rhs->values, context.getLangOpts().isSignedOverflowDefined());
  if (result.alwaysInvalid && !rhs->mayBeInvalid)
    report(makeError(core::diag::InvalidIntegerOperation,
                     "invalid integer operation: " +
                         std::string(core::toString(result.error)),
                     expr));
  const auto lhsExpression = integerExpressionOf(*expr.getLHS(), state);
  const auto rhsExpression = integerExpressionOf(*expr.getRHS(), state);
  std::optional<NumericExpression> expression;
  if (lhsExpression && rhsExpression) {
    const auto promoted = lhsExpression->converted(*computation);
    if (promoted)
      expression = NumericExpression::operation(
          *op, *promoted, *rhsExpression,
          context.getLangOpts().isSignedOverflowDefined());
    if (expression)
      expression = expression->converted(*storage);
  }
  // Hold the result while assignScalar snapshots every old operand under
  // its aliases. The temporary is site-bounded, like an ordinary call output.
  // Intern a stable result slot independently of valueSnapshots' old-value
  // keys.
  auto &outputs = integerStatementResults[&expr];
  if (!outputs) {
    outputs =
        places.create("compound-result@" + std::to_string(locate(expr).line));
    snapshotPlaces.insert(*outputs);
  }
  state.numericValues.erase(*outputs);
  if (expression)
    state.numericValues.insert_or_assign(*outputs, *expression);
  assignScalar(ref->place, nullptr, state, &expr);
  std::vector<core::PlaceId> cells = mirrors(ref->place, state);
  cells.push_back(ref->place);
  llvm::append_range(cells, borrowedImages(ref->place, state));
  for (const auto cell : cells) {
    if (!tracksScalar(cell))
      continue;
    if (!result.mayBeInvalid && !rhs->mayBeInvalid)
      state.scalars.set(
          cell, core::ValueFact::ofInteger(result.values.converted(*storage)));
    if (const auto frozen = state.numericValues.find(*outputs);
        frozen != state.numericValues.end() && !frozen->second.dependsOn(cell))
      state.numericValues.insert_or_assign(cell, frozen->second);
  }
  state.numericValues.erase(*outputs);
}

void FunctionDataflow::applyIntegerRange(const Expr &expr,
                                         const core::IntegerRange &allowed,
                                         core::AnalysisState &state) {
  const auto actual = integerRangeOf(expr, state);
  if (!actual || actual->mayBeInvalid)
    return;
  const auto selected = actual->values.intersect(allowed);
  const auto read = builder.scalarOperand(expr);
  if (selected.empty()) {
    if (!read.place || !places.innermostDeref(read.place->place) ||
        !memoryContext.empty())
      edgeInfeasible = true;
    return;
  }
  const auto fact = core::ValueFact::ofInteger(selected);
  applyOutcomeTest(
      expr, std::set<core::Outcome>(fact.classes.begin(), fact.classes.end()),
      state, fact.constant);
  if (read.place && !read.scaled && read.offset == 0 &&
      read.place->element.isWhole()) {
    const auto *decl =
        dyn_cast_or_null<ValueDecl>(builder.declFor(read.place->place));
    const auto type = decl ? integerTypeOf(*decl, context) : std::nullopt;
    if (type && conversionPreserves(selected, *type)) {
      state.scalars.set(read.place->place,
                        core::ValueFact::ofInteger(selected.converted(*type)));
      learnFact(read.place->place, fact, state);
    }
  }
  if (const auto expression = integerExpressionOf(expr, state)) {
    const core::IntegerPredicate<core::PlaceId> predicate{
        .lhs = *expression,
        .op = core::IntegerOp::Equal,
        .rhs = NumericExpression::constant(
            core::IntegerValue::ofBits(expression->type(), 0)),
        .range = allowed};
    if (!state.numericConditions.requireInteger(predicate) &&
        state.numericConditions.size() >= core::MaxGuardConjuncts)
      state.numericConditionsIncomplete = true;
  } else {
    state.numericConditionsIncomplete = true;
  }
}

void FunctionDataflow::applySwitchEdge(const SwitchStmt &statement,
                                       const CFGBlock &to,
                                       core::AnalysisState &state) {
  const auto *scrutinee = statement.getCond();
  if (!scrutinee)
    return;
  const auto type = integerTypeOf(scrutinee->getType(), context);
  if (!type)
    return;
  const auto labelRange =
      [&](const CaseStmt &label) -> std::optional<core::IntegerRange> {
    const auto lo = integerRangeOf(*label.getLHS(), state);
    const auto hi =
        label.getRHS() ? integerRangeOf(*label.getRHS(), state) : lo;
    if (!lo || !hi || lo->mayBeInvalid || hi->mayBeInvalid)
      return std::nullopt;
    const auto a = lo->values.converted(*type).constant();
    const auto b = hi->values.converted(*type).constant();
    if (!a || !b)
      return std::nullopt;
    return core::IntegerRange::between(*a, *b);
  };
  if (const auto *label = dyn_cast_or_null<CaseStmt>(to.getLabel())) {
    if (const auto range = labelRange(*label))
      applyIntegerRange(*scrutinee, *range, state);
    return;
  }
  if (to.getLabel() && !isa<DefaultStmt>(to.getLabel()))
    return;
  auto allowed = core::IntegerRange::full(*type);
  for (const auto *sc = statement.getSwitchCaseList(); sc;
       sc = sc->getNextSwitchCase()) {
    const auto *label = dyn_cast<CaseStmt>(sc);
    if (!label)
      continue;
    const auto range = labelRange(*label);
    if (!range || range->empty())
      continue;
    // Subtract using ranks, so UINT64_MAX and signed minima never overflow
    // the analyzer. Widening after many labels may retain an impossible edge.
    std::vector<core::IntegerInterval> remaining;
    for (const auto interval : allowed.all()) {
      const auto excluded = range->all().front();
      if (excluded.upper < interval.lower || excluded.lower > interval.upper) {
        remaining.push_back(interval);
        continue;
      }
      if (excluded.lower > interval.lower)
        remaining.push_back(
            {.lower = interval.lower, .upper = excluded.lower - 1});
      if (excluded.upper < interval.upper)
        remaining.push_back(
            {.lower = excluded.upper + 1, .upper = interval.upper});
    }
    allowed = core::IntegerRange::fromRanks(*type, std::move(remaining));
  }
  applyIntegerRange(*scrutinee, allowed, state);
}

} // namespace weavec::analysis
