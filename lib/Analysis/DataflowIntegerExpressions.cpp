//===- DataflowIntegerExpressions.cpp - Symbolic C sizes (RFC 0017) -------===//
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

std::optional<core::Affine>
FunctionDataflow::linearIntegerExpression(const NumericExpression &expression,
                                          const core::AnalysisState &state,
                                          bool upperEnvelope) {
  struct Value {
    core::IntegerRange range;
    std::optional<core::Affine> linear;
    bool upper = false;
  };
  const std::function<Value(const NumericExpression &)> visit =
      [&](const NumericExpression &part) -> Value {
    const auto evaluated = evaluateNumericExpression(part, state);
    Value result{.range = evaluated.values, .linear = std::nullopt};
    if (!evaluated.mayBeInvalid)
      if (const auto value = evaluated.values.constant())
        if (const auto exact = value->signedValue()) {
          result.linear = core::Affine::ofConstant(*exact);
          return result;
        }
    const auto &node = part.all().back();
    if (const auto input = part.inputKey()) {
      result.linear = core::Affine::ofPlace(*input);
      return result;
    }
    const auto operands = part.operands();
    if (operands.empty())
      return result;
    const auto lhs = visit(operands.front());
    if (node.kind == core::IntegerNodeKind::Convert) {
      if (conversionPreserves(lhs.range, node.type) && !lhs.upper)
        result.linear = lhs.linear;
      return result;
    }
    if (node.kind != core::IntegerNodeKind::Operation || operands.size() != 2)
      return result;
    const auto rhs = visit(operands.back());
    if (evaluated.mayBeInvalid || !lhs.linear || !rhs.linear || lhs.upper ||
        rhs.upper)
      return result;
    const bool exact = operationDoesNotOverflow(
        node.op, operands.front(), operands.back(), node.type, state);
    const bool envelope = upperEnvelope && !node.type.isSigned &&
                          node.op == core::IntegerOp::Multiply;
    if (!exact && !envelope)
      return result;
    if (node.op == core::IntegerOp::Add) {
      result.linear = sumOf(*lhs.linear, *rhs.linear);
    } else if (node.op == core::IntegerOp::Subtract &&
               rhs.linear->isConstant() && rhs.linear->constant != INT64_MIN) {
      result.linear = lhs.linear->shifted(-rhs.linear->constant);
    } else if (node.op == core::IntegerOp::Multiply) {
      if (rhs.linear->isConstant() && rhs.linear->constant >= 0)
        result.linear = lhs.linear->times(rhs.linear->constant);
      else if (lhs.linear->isConstant() && lhs.linear->constant >= 0)
        result.linear = rhs.linear->times(lhs.linear->constant);
    }
    result.upper = !exact;
    return result;
  };
  return visit(expression).linear;
}

std::optional<FunctionDataflow::NumericExpression>
FunctionDataflow::integerExpressionOf(const Expr &expr,
                                      const core::AnalysisState &state,
                                      unsigned depth) {
  const auto type = integerTypeOf(expr.getType(), context);
  if (!type || expr.isValueDependent())
    return std::nullopt;
  if (depth >= core::MaxIntegerExpressionDepth) {
    reportIncomplete("integer expression limit reached", expr);
    return std::nullopt;
  }
  const Expr *e = expr.IgnoreParens();
  const auto child = [&](const Expr &operand) {
    return integerExpressionOf(operand, state, depth + 1);
  };
  if (const auto *cast = dyn_cast<CastExpr>(e)) {
    const auto value = child(*cast->getSubExpr());
    const auto converted = value ? value->converted(*type) : std::nullopt;
    if (value && !converted)
      reportIncomplete("integer expression limit reached", expr);
    return converted;
  }
  if (const auto *constant = dyn_cast<ConstantExpr>(e))
    return child(*constant->getSubExpr());
  if (const auto *trait = dyn_cast<UnaryExprOrTypeTraitExpr>(e);
      trait && trait->getKind() == UETT_SizeOf &&
      trait->getTypeOfArgument()->isVariablyModifiedType())
    return variableArraySize(trait->getTypeOfArgument(), state);
  if (const auto *binary = dyn_cast<BinaryOperator>(e)) {
    if (binary->getOpcode() == BO_Assign || binary->getOpcode() == BO_Comma)
      return child(*binary->getRHS());
    if (binary->isCompoundAssignmentOp())
      return child(*binary->getLHS());
    const auto op = integerOpOf(binary->getOpcode());
    const auto lhs = child(*binary->getLHS());
    const auto rhs = child(*binary->getRHS());
    if (!op || !lhs || !rhs)
      return std::nullopt;
    const auto value = NumericExpression::operation(
        *op, *lhs, *rhs, context.getLangOpts().isSignedOverflowDefined());
    if (!value)
      reportIncomplete("integer expression limit reached", expr);
    return value ? value->converted(*type) : std::nullopt;
  }
  // A dereference is an integer place read. Its pointer operand is not an
  // integer expression; let the place path below resolve and read the cell.
  if (const auto *unary = dyn_cast<UnaryOperator>(e);
      unary && unary->getOpcode() != UO_Deref) {
    auto value = child(*unary->getSubExpr());
    if (!value)
      return std::nullopt;
    if (unary->getOpcode() == UO_Plus)
      return value;
    if (unary->isIncrementDecrementOp()) {
      if (!unary->isPostfix())
        return value;
      if (const auto saved = integerStatementResults.find(unary);
          saved != integerStatementResults.end() && saved->second) {
        if (const auto old = state.numericValues.find(*saved->second);
            old != state.numericValues.end())
          return old->second.converted(*type);
      }
      const auto ref = builder.resolve(*unary->getSubExpr());
      const auto *decl =
          ref ? dyn_cast_or_null<ValueDecl>(builder.declFor(ref->place))
              : nullptr;
      const auto storage = decl ? integerTypeOf(*decl, context) : type;
      if (!storage || storage->isBoolean)
        return std::nullopt;
      const auto stored = value->converted(*storage);
      if (!stored)
        return std::nullopt;
      const auto previous = NumericExpression::operation(
          unary->isIncrementOp() ? core::IntegerOp::Subtract
                                 : core::IntegerOp::Add,
          *stored,
          NumericExpression::constant(core::IntegerValue::ofBits(*storage, 1)),
          true);
      return previous ? previous->converted(*type) : std::nullopt;
    }
    std::optional<core::IntegerOp> op;
    if (unary->getOpcode() == UO_Minus)
      op = core::IntegerOp::Negate;
    if (unary->getOpcode() == UO_Not)
      op = core::IntegerOp::Complement;
    if (unary->getOpcode() == UO_LNot)
      op = core::IntegerOp::LogicalNot;
    if (!op)
      return std::nullopt;
    const auto result = NumericExpression::operation(
        *op, *value, *value, context.getLangOpts().isSignedOverflowDefined());
    return result ? result->converted(*type) : std::nullopt;
  }
  if (const auto *conditional = dyn_cast<AbstractConditionalOperator>(e)) {
    const auto condition = integerRangeOf(*conditional->getCond(), state);
    if (condition && !condition->mayBeInvalid) {
      if (const auto value =
              condition->values.converted(core::BooleanType).constant())
        return child(value->bits != 0 ? *conditional->getTrueExpr()
                                      : *conditional->getFalseExpr());
    }
    auto a = child(*conditional->getTrueExpr());
    const auto b = child(*conditional->getFalseExpr());
    if (!a || !b)
      return std::nullopt;
    if (*a == *b)
      return a;
    const auto *comparison =
        dyn_cast<BinaryOperator>(conditional->getCond()->IgnoreParenImpCasts());
    if (!comparison || !comparison->isRelationalOp())
      return std::nullopt;
    const auto lhs = child(*comparison->getLHS());
    const auto rhs = child(*comparison->getRHS());
    if (!lhs || !rhs)
      return std::nullopt;
    const bool less =
        comparison->getOpcode() == BO_LT || comparison->getOpcode() == BO_LE;
    const bool direct = *lhs == *a && *rhs == *b;
    const bool reverse = *lhs == *b && *rhs == *a;
    if (!direct && !reverse)
      return std::nullopt;
    return NumericExpression::operation(
        less == direct ? core::IntegerOp::Minimum : core::IntegerOp::Maximum,
        *a, *b);
  }
  if (PlaceBuilder::isPlaceExpr(*e)) {
    const auto *ref = dyn_cast<DeclRefExpr>(e);
    if (!ref || !isa<EnumConstantDecl>(ref->getDecl())) {
      if (const auto place = builder.resolve(*e);
          place && place->element.isWhole()) {
        if (const auto stored = state.numericValues.find(place->place);
            stored != state.numericValues.end())
          return stored->second.converted(*type);
        return NumericExpression::input(place->place, *type);
      }
    }
  }
  if (weavec::analysis::PlaceBuilder::strlenArgumentOf(*e)) {
    const auto length = builder.legacyAffineOf(*e);
    if (length && length->place && length->scale == 1 && length->constant == 0)
      return NumericExpression::input(*length->place, *type);
  }
  if (const auto *call = dyn_cast<CallExpr>(e))
    if (const auto result = numericCallResult(*call)) {
      if (const auto stored = state.numericValues.find(*result);
          stored != state.numericValues.end())
        return stored->second.converted(*type);
      return NumericExpression::input(*result, *type);
    }
  Expr::EvalResult evaluated;
  if (!isa<CallExpr, AtomicExpr>(e) && e->EvaluateAsInt(evaluated, context) &&
      evaluated.Val.isInt())
    return NumericExpression::constant(core::IntegerValue::ofBits(
        *type, evaluated.Val.getInt().zextOrTrunc(type->width).getZExtValue()));
  return std::nullopt;
}

core::Affine
FunctionDataflow::internIntegerExpression(const NumericExpression &expression,
                                          core::AnalysisState &state) {
  const auto evaluated = evaluateNumericExpression(expression, state);
  if (!evaluated.mayBeInvalid) {
    if (const auto value = evaluated.values.constant()) {
      if (const auto exact = value->signedValue())
        return core::Affine::ofConstant(*exact);
    }
  }
  if (const auto input = expression.inputKey())
    return core::Affine::ofPlace(*input);
  auto entry = expressionPlaces.find(expression);
  if (entry == expressionPlaces.end()) {
    const auto name =
        expression.describe([&](core::PlaceId place) { return nameOf(place); });
    const auto place = places.create(name);
    entry = expressionPlaces.emplace(expression, place).first;
    numericExpressions.emplace(place, expression);
  }
  if (const auto changed = expression.differsFromInput();
      changed && !evaluated.alwaysInvalid)
    state.relations.requireDifferent(entry->second, *changed);
  if (!evaluated.mayBeInvalid) {
    const auto fact = core::ValueFact::ofInteger(evaluated.values);
    state.scalars.set(entry->second, fact);
  } else {
    state.scalars.forget(entry->second);
  }
  return core::Affine::ofPlace(entry->second);
}

std::optional<core::Affine>
FunctionDataflow::integerAffineOf(const Expr &expr,
                                  core::AnalysisState &state) {
  if (const auto range = integerRangeOf(expr, state);
      range && !range->mayBeInvalid) {
    if (const auto value = range->values.constant())
      if (const auto exact = value->signedValue())
        return core::Affine::ofConstant(*exact);
  }
  const Expr *e = expr.IgnoreParens();
  if (const auto *cast = dyn_cast<CastExpr>(e);
      cast && preservesInteger(*cast, state))
    return integerAffineOf(*cast->getSubExpr(), state);
  if (PlaceBuilder::isPlaceExpr(*e))
    if (const auto place = builder.resolve(*e);
        place && numericEntryValues.contains(place->place))
      if (const auto expression = integerExpressionOf(*e, state))
        return internIntegerExpression(*expression, state);
  if (PlaceBuilder::isPlaceExpr(*e) ||
      weavec::analysis::PlaceBuilder::strlenArgumentOf(*e))
    return builder.legacyAffineOf(*e);
  if (const auto *binary = dyn_cast<BinaryOperator>(e);
      binary && preservesInteger(*binary, state))
    if (const auto linear = builder.legacyAffineOf(*binary))
      return linear;
  const auto expression = integerExpressionOf(expr, state);
  return expression ? std::optional(internIntegerExpression(*expression, state))
                    : std::nullopt;
}

std::optional<core::Affine>
FunctionDataflow::instantiateIntegerExpression(const core::PathAffine &value,
                                               const CallExpr &call,
                                               core::AnalysisState &state) {
  if (!value.expression)
    return std::nullopt;
  const auto substituted = value.expression->substitute<core::PlaceId>(
      [&](const core::SummaryPath &path,
          core::IntegerType type) -> std::optional<NumericExpression> {
        return numericInput(call, path, type, state);
      });
  if (!substituted)
    return std::nullopt;
  const auto scaled =
      internIntegerExpression(*substituted, state).times(value.scale);
  return scaled ? scaled->shifted(value.constant) : std::nullopt;
}

std::optional<core::IntegerExpression<core::SummaryPath>>
FunctionDataflow::summaryIntegerExpression(
    const NumericExpression &expression) {
  return expression.substitute<core::SummaryPath>(
      [&](core::PlaceId place, core::IntegerType type)
          -> std::optional<core::IntegerExpression<core::SummaryPath>> {
        if (const auto saved = numericSnapshotExpressions.find(place);
            saved != numericSnapshotExpressions.end())
          return saved->second.converted(type);
        const auto path = stableSummaryPathOf(place);
        if (!path ||
            (currentState && currentState->numericWrites.contains(place)))
          return std::nullopt;
        return core::IntegerExpression<core::SummaryPath>::input(*path, type);
      });
}

void FunctionDataflow::snapshotIntegerDependencies(core::PlaceId place,
                                                   const Expr *at,
                                                   core::AnalysisState &state) {
  for (const auto &[symbol, expression] : numericExpressions) {
    if (!expression.dependsOn(place))
      continue;
    const auto range = evaluateNumericExpression(expression, state);
    if (!range.mayBeInvalid)
      state.scalars.set(symbol, core::ValueFact::ofInteger(range.values));
    else
      state.scalars.forget(symbol);
    snapshotScalar(symbol, at, state);
    if (const auto saved = valueSnapshots.find({symbol, at});
        saved != valueSnapshots.end()) {
      numericSnapshotExpressions.erase(saved->second);
      if (const auto projected = summaryIntegerExpression(expression))
        numericSnapshotExpressions.emplace(saved->second, *projected);
    }
    state.relations.forget(symbol);
    state.scalars.forget(symbol);
    state.dropGuardsOn(symbol);
  }
}

} // namespace weavec::analysis
