//===- DataflowIntegers.cpp - C integer values in the checker
//--------------===//
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

core::IntegerRange
FunctionDataflow::integerRangeAt(core::PlaceId place, core::IntegerType type,
                                 const core::AnalysisState &state) {
  auto range = core::IntegerRange::full(type);
  if (builder.isLengthPlace(place)) {
    const auto sizeType = integerTypeOf(context.getSizeType(), context);
    if (sizeType)
      range = core::IntegerRange::between(
                  core::IntegerValue::ofBits(*sizeType, 0),
                  core::IntegerValue::ofBits(*sizeType, sizeType->mask() - 1))
                  .converted(type);
  }
  if (const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(place)))
    if (const auto storage = integerTypeOf(*decl, context))
      range = core::IntegerRange::full(*storage).converted(type);
  if (const auto fact = state.scalars.factOf(place))
    range = range.intersect(fact->inType(type));
  const auto restrict = [&](std::optional<std::int64_t> bound,
                            core::IntegerOp op) {
    if (!bound)
      return;
    const auto signedValue = core::IntegerValue::ofBits(
        {.width = 64, .isSigned = true}, static_cast<std::uint64_t>(*bound));
    const auto value = signedValue.converted(type);
    if (!sameIntegerValue(signedValue, value))
      return;
    range = range.satisfying(op, core::IntegerRange::singleton(value));
  };
  restrict(state.relations.atLeast(place), core::IntegerOp::GreaterEqual);
  restrict(state.relations.atMost(place), core::IntegerOp::LessEqual);
  // Widening at a loop-body join may forget the target maximum excluded by
  // `i < n`. Reapply one-hop relations before using i++ as C arithmetic;
  // this is not a reachable-boundary claim for arbitrary array indices.
  for (const auto &[pair, stored] : state.relations.all()) {
    if (pair.first != place && pair.second != place)
      continue;
    const auto edge =
        pair.first == place ? std::optional(stored) : stored.flipped();
    if (!edge)
      continue;
    const auto other = pair.first == place ? pair.second : pair.first;
    const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(other));
    const auto otherType = decl ? integerTypeOf(*decl, context) : std::nullopt;
    if (!otherType)
      continue;
    auto otherRange = core::IntegerRange::full(*otherType);
    if (const auto fact = state.scalars.factOf(other))
      otherRange = otherRange.intersect(fact->inType(*otherType));
    if (otherRange.empty())
      continue;
    const auto shifted = [&](std::optional<std::int64_t> value,
                             int extra) -> std::optional<std::int64_t> {
      if (!value || __builtin_add_overflow(*value, edge->offset, &*value) ||
          __builtin_add_overflow(*value, extra, &*value))
        return std::nullopt;
      return value;
    };
    if (edge->relation == core::Relation::Less ||
        edge->relation == core::Relation::LessEqual ||
        edge->relation == core::Relation::Equal)
      restrict(shifted(otherRange.maximum()->signedValue(),
                       edge->relation == core::Relation::Less ? -1 : 0),
               core::IntegerOp::LessEqual);
    if (edge->relation == core::Relation::Greater ||
        edge->relation == core::Relation::GreaterEqual ||
        edge->relation == core::Relation::Equal)
      restrict(shifted(otherRange.minimum()->signedValue(),
                       edge->relation == core::Relation::Greater ? 1 : 0),
               core::IntegerOp::GreaterEqual);
  }
  return range;
}

std::optional<core::IntegerRangeEvaluation> FunctionDataflow::integerRangeOf(
    const Expr &expr, const core::AnalysisState &state, unsigned depth) {
  const auto type = integerTypeOf(expr.getType(), context);
  if (!type)
    return std::nullopt;
  const auto unknown = [type]() -> core::IntegerRangeEvaluation {
    return {.values = core::IntegerRange::full(*type)};
  };
  if (depth >= 12 || expr.isValueDependent())
    return unknown();
  const Expr *e = expr.IgnoreParens();
  const auto child = [&](const Expr &operand) {
    return integerRangeOf(operand, state, depth + 1);
  };
  const auto converted = [type](core::IntegerRangeEvaluation result) {
    result.values = result.values.converted(*type);
    return result;
  };
  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    const auto source = child(*cast->getSubExpr());
    return source ? converted(*source) : unknown();
  }
  if (const auto *constant = dyn_cast<ConstantExpr>(e))
    return child(*constant->getSubExpr());
  if (const auto *trait = dyn_cast<UnaryExprOrTypeTraitExpr>(e);
      trait && trait->getKind() == UETT_SizeOf &&
      trait->getTypeOfArgument()->isVariablyModifiedType()) {
    const auto size = variableArraySize(trait->getTypeOfArgument(), state);
    return size ? converted(size->evaluate(
                      [&](core::PlaceId place, core::IntegerType inputType) {
                        return integerRangeAt(place, inputType, state);
                      }))
                : unknown();
  }
  if (const auto *binary = dyn_cast<BinaryOperator>(e)) {
    if (binary->getOpcode() == BO_Assign || binary->getOpcode() == BO_Comma)
      return child(*binary->getRHS());
    if (binary->isCompoundAssignmentOp()) {
      const auto ref = builder.resolve(*binary->getLHS());
      return ref ? core::IntegerRangeEvaluation{.values = integerRangeAt(
                                                    ref->place, *type, state)}
                 : unknown();
    }
    const auto lhs = child(*binary->getLHS());
    const auto rhs = child(*binary->getRHS());
    if (!lhs || !rhs)
      return unknown();
    if (binary->isLogicalOp()) {
      const auto a = lhs->values.converted(core::BooleanType);
      const auto b = rhs->values.converted(core::BooleanType);
      auto result = core::evaluateInteger(binary->getOpcode() == BO_LAnd
                                              ? core::IntegerOp::BitAnd
                                              : core::IntegerOp::BitOr,
                                          a, b);
      result.mayBeInvalid |= lhs->mayBeInvalid || rhs->mayBeInvalid;
      if (result.mayBeInvalid)
        result.values = core::IntegerRange::full(core::BooleanType);
      return converted(result);
    }
    const auto op = integerOpOf(binary->getOpcode());
    if (!op)
      return unknown();
    const bool wrapping = context.getLangOpts().isSignedOverflowDefined();
    auto result =
        core::evaluateInteger(*op, lhs->values, rhs->values, wrapping);
    if (!lhs->mayBeInvalid && !rhs->mayBeInvalid &&
        !state.numericConditions.integers.empty() &&
        (*op == core::IntegerOp::Add || *op == core::IntegerOp::Subtract ||
         *op == core::IntegerOp::Multiply)) {
      const auto expression = integerExpressionOf(*binary, state);
      if (expression)
        result = evaluateNumericExpression(*expression, state);
    }
    if (lhs->mayBeInvalid || rhs->mayBeInvalid) {
      result.values = core::IntegerRange::full(result.values.type);
      result.mayBeInvalid = true;
      // A child's error is reported at that child, never at every parent.
      result.alwaysInvalid = false;
    }
    return converted(result);
  }
  // Read an integer dereference through the place case below, without trying
  // to evaluate its pointer operand in the integer domain.
  if (const auto *unary = dyn_cast<UnaryOperator>(e);
      unary && unary->getOpcode() != UO_Deref) {
    if (unary->isIncrementDecrementOp()) {
      if (unary->isPostfix())
        if (const auto saved = integerStatementResults.find(unary);
            saved != integerStatementResults.end() && saved->second)
          return core::IntegerRangeEvaluation{
              .values = integerRangeAt(*saved->second, *type, state)};
      const auto ref = builder.resolve(*unary->getSubExpr());
      if (!ref)
        return unknown();
      auto range = integerRangeAt(ref->place, *type, state);
      if (!unary->isPostfix())
        return core::IntegerRangeEvaluation{.values = std::move(range)};
      const auto *decl =
          dyn_cast_or_null<ValueDecl>(builder.declFor(ref->place));
      const auto storage = decl ? integerTypeOf(*decl, context) : type;
      if (!storage || storage->isBoolean)
        return unknown();
      // State has the new value; postfix yields the previous value. Reverse
      // the assignment's conversion modulo its storage width, not host int.
      const auto one = core::IntegerRange::singleton(
          core::IntegerValue::ofBits(*storage, 1));
      return converted(core::evaluateInteger(
          unary->isIncrementOp() ? core::IntegerOp::Subtract
                                 : core::IntegerOp::Add,
          range.converted(*storage), one, true));
    }
    const auto source = child(*unary->getSubExpr());
    if (!source)
      return unknown();
    if (unary->getOpcode() == UO_Plus)
      return converted(*source);
    std::optional<core::IntegerOp> op;
    switch (unary->getOpcode()) {
    case UO_Minus:
      op = core::IntegerOp::Negate;
      break;
    case UO_Not:
      op = core::IntegerOp::Complement;
      break;
    case UO_LNot:
      op = core::IntegerOp::LogicalNot;
      break;
    default:
      break;
    }
    if (op) {
      auto result = core::evaluateInteger(
          *op, source->values, source->values,
          context.getLangOpts().isSignedOverflowDefined());
      if (source->mayBeInvalid) {
        result.values = core::IntegerRange::full(result.values.type);
        result.mayBeInvalid = true;
        result.alwaysInvalid = false;
      }
      return converted(result);
    }
  }
  if (const auto *conditional = dyn_cast<AbstractConditionalOperator>(e)) {
    const auto condition = child(*conditional->getCond());
    if (condition && !condition->mayBeInvalid) {
      if (const auto value =
              condition->values.converted(core::BooleanType).constant())
        return child(value->bits != 0 ? *conditional->getTrueExpr()
                                      : *conditional->getFalseExpr());
    }
    const auto a = child(*conditional->getTrueExpr());
    const auto b = child(*conditional->getFalseExpr());
    if (!a || !b)
      return unknown();
    return core::IntegerRangeEvaluation{
        .values = a->values.converted(*type).united(b->values.converted(*type)),
        .mayBeInvalid = a->mayBeInvalid || b->mayBeInvalid};
  }
  if (PlaceBuilder::isPlaceExpr(*e)) {
    if (const auto *ref = dyn_cast<DeclRefExpr>(e);
        ref && isa<EnumConstantDecl>(ref->getDecl())) {
      const auto &value = cast<EnumConstantDecl>(ref->getDecl())->getInitVal();
      return core::IntegerRangeEvaluation{
          .values = core::IntegerRange::singleton(core::IntegerValue::ofBits(
              *type, value.zextOrTrunc(type->width).getZExtValue()))};
    }
    const auto ref = builder.resolve(*e);
    if (ref && ref->element.isWhole()) {
      auto range = integerRangeAt(ref->place, *type, state);
      if (!state.numericConditions.integers.empty())
        if (const auto value = state.numericValues.find(ref->place);
            value != state.numericValues.end()) {
          const auto evaluated =
              evaluateNumericExpression(value->second, state);
          if (!evaluated.mayBeInvalid)
            range = range.intersect(evaluated.values.converted(*type));
        }
      return core::IntegerRangeEvaluation{.values = range};
    }
    return unknown();
  }
  if (weavec::analysis::PlaceBuilder::strlenArgumentOf(*e))
    if (const auto length = builder.legacyAffineOf(*e);
        length && length->place && length->scale == 1 && length->constant == 0)
      return core::IntegerRangeEvaluation{
          .values = integerRangeAt(*length->place, *type, state)};
  if (const auto *call = dyn_cast<CallExpr>(e))
    if (const auto result = numericCallResult(*call))
      return core::IntegerRangeEvaluation{
          .values = integerRangeAt(*result, *type, state)};
  // Constant leaves (including target sizeof/alignof). Arithmetic nodes were
  // handled above so Clang's fold cannot hide an invalid integer operation.
  Expr::EvalResult result;
  if (!isa<CallExpr, AtomicExpr>(e) && e->EvaluateAsInt(result, context) &&
      result.Val.isInt()) {
    const auto bits =
        result.Val.getInt().zextOrTrunc(type->width).getZExtValue();
    return core::IntegerRangeEvaluation{
        .values = core::IntegerRange::singleton(
            core::IntegerValue::ofBits(*type, bits))};
  }
  if (const auto adjustment = builder.adjustmentOf(*e)) {
    auto range = integerRangeAt(adjustment->place.place, *type, state);
    if (adjustment->valueOffset == 0)
      return core::IntegerRangeEvaluation{.values = std::move(range)};
    const auto one =
        core::IntegerRange::singleton(core::IntegerValue::ofBits(*type, 1));
    return core::evaluateInteger(adjustment->valueOffset > 0
                                     ? core::IntegerOp::Add
                                     : core::IntegerOp::Subtract,
                                 range, one, true);
  }
  return unknown();
}

bool FunctionDataflow::preservesInteger(const Expr &expr,
                                        const core::AnalysisState &state) {
  if (const auto *cast = dyn_cast<CastExpr>(expr.IgnoreParens())) {
    const auto destination = integerTypeOf(cast->getType(), context);
    const auto source = integerRangeOf(*cast->getSubExpr(), state);
    return destination && source && !source->mayBeInvalid &&
           conversionPreserves(source->values, *destination);
  }
  const auto *binary = dyn_cast<BinaryOperator>(expr.IgnoreParens());
  if (!binary)
    return false;
  const auto type = integerTypeOf(binary->getType(), context);
  const auto lhs = integerRangeOf(*binary->getLHS(), state);
  const auto rhs = integerRangeOf(*binary->getRHS(), state);
  const auto op = integerOpOf(binary->getOpcode());
  if (!type || !lhs || !rhs || !op || lhs->mayBeInvalid || rhs->mayBeInvalid ||
      lhs->values.empty() || rhs->values.empty())
    return false;
  if (!state.numericConditions.integers.empty()) {
    const auto a = integerExpressionOf(*binary->getLHS(), state);
    const auto b = integerExpressionOf(*binary->getRHS(), state);
    if (a && b && operationDoesNotOverflow(*op, *a, *b, *type, state))
      return true;
  }
  if (type->isSigned) {
    const auto result =
        core::evaluateInteger(*op, lhs->values, rhs->values, false);
    return !result.mayBeInvalid;
  }
  const auto a = lhs->values.maximum()->bits;
  const auto b = rhs->values.maximum()->bits;
  switch (*op) {
  case core::IntegerOp::Add:
    return a <= type->mask() - b;
  case core::IntegerOp::Subtract:
    return lhs->values.minimum()->bits >= b;
  case core::IntegerOp::Multiply:
    return b == 0 || a <= type->mask() / b;
  default:
    return false;
  }
}

void FunctionDataflow::checkIntegerOperation(const Expr &expr,
                                             core::AnalysisState &state) {
  if (!expr.getType()->isIntegerType())
    return;
  if (!integerTypeOf(expr.getType(), context)) {
    reportIncomplete("unsupported integer width greater than 64 bits", expr);
    return;
  }
  const auto *binary = dyn_cast<BinaryOperator>(&expr);
  const auto *unary = dyn_cast<UnaryOperator>(&expr);
  if ((!binary || binary->isAssignmentOp() || binary->isComparisonOp()) &&
      (!unary || unary->isIncrementDecrementOp()))
    return;
  const auto result = integerRangeOf(expr, state);
  if (!result || !result->alwaysInvalid ||
      result->error == core::IntegerError::IncompatibleTypes)
    return;
  report(makeError(core::diag::InvalidIntegerOperation,
                   "invalid integer operation: " +
                       std::string(core::toString(result->error)),
                   expr));
}

bool FunctionDataflow::refineIntegerComparison(const Expr &lhs,
                                               BinaryOperatorKind op,
                                               const Expr &rhs, bool holds,
                                               core::AnalysisState &state) {
  const auto operation = integerOpOf(op);
  const auto a = integerRangeOf(lhs, state);
  const auto b = integerRangeOf(rhs, state);
  if (!operation)
    return false;
  const auto selected = holds ? *operation : core::negateComparison(*operation);
  recordIntegerCondition(lhs, selected, rhs, state);
  if (!a || !b || a->mayBeInvalid || b->mayBeInvalid)
    return false;
  const auto narrowed = a->values.satisfying(selected, b->values);
  const auto left = builder.scalarOperand(lhs);
  const auto right = builder.scalarOperand(rhs);
  const auto trusted = [&](const PlaceBuilder::ScalarOperand &read) {
    return !read.place || !places.innermostDeref(read.place->place) ||
           !memoryContext.empty();
  };
  if (narrowed.empty()) {
    if (trusted(left) && trusted(right))
      edgeInfeasible = true;
    return true;
  }
  const auto learn = [&](const PlaceBuilder::ScalarOperand &read,
                         const core::IntegerRange &range) {
    if (!read.place || read.scaled || read.offset != 0 ||
        !read.place->element.isWhole() || !tracksScalar(read.place->place))
      return;
    const auto *decl =
        dyn_cast_or_null<ValueDecl>(builder.declFor(read.place->place));
    const auto type = decl ? integerTypeOf(*decl, context)
                           : std::optional<core::IntegerType>();
    if (!type || range.isFull() || !conversionPreserves(range, *type))
      return;
    const auto fact = core::ValueFact::ofInteger(range.converted(*type));
    if (!fact.trivial()) {
      state.scalars.set(read.place->place, fact);
      learnFact(read.place->place, fact, state);
    }
  };
  learn(left, narrowed);
  learn(right,
        b->values.satisfying(core::reverseComparison(selected), a->values));
  return true;
}

std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>
FunctionDataflow::integerBounds(core::PlaceId place,
                                const core::AnalysisState &state) {
  auto lower = state.relations.atLeast(place);
  auto upper = state.relations.atMost(place);
  const auto *decl = dyn_cast_or_null<ValueDecl>(builder.declFor(place));
  auto type = decl ? integerTypeOf(*decl, context) : std::nullopt;
  if (const auto symbolic = numericExpressions.find(place);
      symbolic != numericExpressions.end())
    type = symbolic->second.type();
  const auto fact = state.scalars.factOf(place);
  if (!type && fact && fact->integer)
    type = fact->integer->type;
  if (!type)
    return {lower, upper};
  const auto range = integerRangeAt(place, *type, state);
  if (range.empty())
    return {lower, upper};
  if (const auto minimum = range.minimum()->signedValue())
    lower = lower ? std::max(*lower, *minimum) : minimum;
  // The target's maximum alone is not a reachable boundary established by
  // source constraints. Using it as one would diagnose every unknown index.
  if (range.maximum() != core::IntegerRange::full(*type).maximum())
    if (const auto maximum = range.maximum()->signedValue())
      upper = upper ? std::min(*upper, *maximum) : maximum;
  return {lower, upper};
}

void FunctionDataflow::recordSpatialCheck(const Expr &at,
                                          core::SpatialCheck check) {
  if (!recording())
    return;
  auto [it, added] = spatialChecks.try_emplace(&at, check);
  if (added || it->second.outcome == core::SpatialOutcome::Violation)
    return;
  // Multiple constraints at the same source operation must all hold. A
  // placeholder for unknown information can be completed by a later check.
  if (check.outcome == core::SpatialOutcome::Violation ||
      it->second.reason == core::SpatialReason::UnknownExtent ||
      it->second.reason == core::SpatialReason::UnsupportedExpression ||
      check.outcome == core::SpatialOutcome::Unresolved)
    it->second = check;
}

void FunctionDataflow::recordIntegerCondition(const Expr &lhs,
                                              core::IntegerOp op,
                                              const Expr &rhs,
                                              core::AnalysisState &state) {
  const auto a = integerExpressionOf(lhs, state);
  const auto b = integerExpressionOf(rhs, state);
  if (!a || !b) {
    state.numericConditionsIncomplete = true;
    return;
  }
  const core::IntegerPredicate<core::PlaceId> predicate{
      .lhs = *a, .op = op, .rhs = *b};
  if (const auto known =
          predicate.evaluate([&](core::PlaceId place, core::IntegerType type) {
            return integerRangeAt(place, type, state);
          });
      known && *known)
    return;
  if (state.numericConditions.size() >= core::MaxGuardConjuncts &&
      !std::ranges::binary_search(state.numericConditions.integers,
                                  predicate)) {
    state.numericConditionsIncomplete = true;
    return;
  }
  state.numericConditions.requireInteger(predicate);
}

std::optional<core::PlaceGuard>
FunctionDataflow::translateIntegerGuard(const core::PathGuard &guard,
                                        const CallExpr &call,
                                        const core::AnalysisState &state) {
  core::PlaceGuard translated;
  for (const auto &predicate : guard.integers) {
    const auto mapped = predicate.substitute<core::PlaceId>(
        [&](const core::SummaryPath &path,
            core::IntegerType type) -> std::optional<NumericExpression> {
          return numericInput(call, path, type, state);
        });
    if (!mapped) {
      reportIncomplete("unsupported numeric condition projection", call);
      auto &unknown = integerStatementResults[&call];
      if (!unknown) {
        unknown = places.create("unresolved-integer-condition@" +
                                std::to_string(locate(call).line));
        snapshotPlaces.insert(*unknown);
      }
      // Preserve an undecided premise for must-requirements. A may-effect
      // still applies, while a caller cannot report by deleting this premise.
      translated.requireInteger(
          {.lhs = NumericExpression::input(*unknown, core::BooleanType),
           .op = core::IntegerOp::Equal,
           .rhs = NumericExpression::constant(
               core::IntegerValue::ofBits(core::BooleanType, 1))});
      continue;
    }
    const auto known =
        mapped->evaluate([&](core::PlaceId place, core::IntegerType type) {
          return integerRangeAt(place, type, state);
        });
    if (known && !*known)
      return std::nullopt;
    if (!known)
      translated.requireInteger(*mapped);
  }
  return translated;
}

} // namespace weavec::analysis
