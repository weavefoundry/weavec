//===- DataflowIntegerProofs.cpp - Checked arithmetic facts (RFC 0017)
//-----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dataflow.h"
#include "IntegerSupport.h"

namespace weavec::analysis {

bool FunctionDataflow::operationDoesNotOverflow(
    core::IntegerOp op, const NumericExpression &lhs,
    const NumericExpression &rhs, core::IntegerType type,
    const core::AnalysisState &state) {
  if (op != core::IntegerOp::Add && op != core::IntegerOp::Subtract &&
      op != core::IntegerOp::Multiply)
    return false;
  const auto read = [&](core::PlaceId place, core::IntegerType inputType) {
    return integerRangeAt(place, inputType, state);
  };
  const auto a = lhs.evaluate(read);
  const auto b = rhs.evaluate(read);
  if (a.mayBeInvalid || b.mayBeInvalid || a.values.empty() || b.values.empty())
    return false;
  if (const auto overflow =
          core::evaluateCheckedInteger(op, a.values, b.values, type)
              .overflow.constant();
      overflow && overflow->bits == 0)
    return true;
  const auto overflowExpression =
      NumericExpression::overflow(op, lhs, rhs, type);
  for (const auto &predicate : state.numericConditions.integers) {
    auto tested = predicate.lhs;
    auto limit = predicate.rhs;
    auto comparison = predicate.op;
    if (tested.constantValue() && !limit.constantValue()) {
      std::swap(tested, limit);
      comparison = core::reverseComparison(comparison);
    }
    // An overflow builtin returns a boolean, possibly promoted before it
    // is tested or returned by a helper. Only value-preserving conversions
    // may be peeled from its result.
    while (tested.all().back().kind == core::IntegerNodeKind::Convert) {
      const auto operand = tested.operands().front();
      const auto range = operand.evaluate(read);
      if (range.mayBeInvalid ||
          !conversionPreserves(range.values, tested.type()))
        break;
      tested = operand;
    }
    const auto zero = limit.constantValue();
    const auto selected =
        predicate.range ? predicate.range->constant() : std::nullopt;
    const bool isZero =
        predicate.range
            ? selected && selected->bits == 0
            : zero &&
                  ((comparison == core::IntegerOp::Equal && zero->bits == 0) ||
                   (tested.type().isBoolean &&
                    comparison == core::IntegerOp::NotEqual &&
                    zero->bits == 1));
    if (isZero && overflowExpression && tested == *overflowExpression)
      return true;
    if (op != core::IntegerOp::Multiply || predicate.range)
      continue;
    // `n <= MAX / m`, with n >= 0 and m > 0, is a mathematical
    // multiplication proof. The MAX and operands must have exactly the
    // operation's type: narrowing a guard does not check a wider product.
    if (comparison == core::IntegerOp::GreaterEqual ||
        comparison == core::IntegerOp::Greater) {
      std::swap(tested, limit);
      comparison = core::reverseComparison(comparison);
    }
    if (comparison != core::IntegerOp::LessEqual &&
        comparison != core::IntegerOp::Less)
      continue;
    if (limit.type() != type || tested.type() != type ||
        limit.all().back().kind != core::IntegerNodeKind::Operation ||
        limit.all().back().op != core::IntegerOp::Divide)
      continue;
    const auto operands = limit.operands();
    const auto maximum = operands.front().constantValue();
    const auto maxBits = type.isSigned ? type.mask() >> 1U : type.mask();
    if (!maximum || maximum->bits != maxBits ||
        !((tested == lhs && operands.back() == rhs) ||
          (tested == rhs && operands.back() == lhs)))
      continue;
    const auto divisor = operands.back().evaluate(read);
    if (!a.values.minimum()->negative() && !b.values.minimum()->negative() &&
        !divisor.mayBeInvalid && !divisor.values.empty() &&
        !divisor.values.minimum()->negative() &&
        divisor.values.minimum()->bits != 0)
      return true;
  }
  return false;
}

core::IntegerRangeEvaluation
FunctionDataflow::evaluateNumericExpression(const NumericExpression &expression,
                                            const core::AnalysisState &state) {
  const auto read = [&](core::PlaceId place, core::IntegerType type) {
    return integerRangeAt(place, type, state);
  };
  if (state.numericConditions.integers.empty())
    return expression.evaluate(read);
  const auto &root = expression.all().back();
  const auto operands = expression.operands();
  if (operands.empty())
    return expression.evaluate(read);
  auto lhs = evaluateNumericExpression(operands.front(), state);
  if (root.kind == core::IntegerNodeKind::Convert) {
    lhs.values = lhs.values.converted(root.type);
    return lhs;
  }
  const auto rhs = operands.size() == 1
                       ? lhs
                       : evaluateNumericExpression(operands.back(), state);
  auto result =
      root.kind == core::IntegerNodeKind::Overflow
          ? core::IntegerRangeEvaluation{.values =
                                             core::evaluateCheckedInteger(
                                                 root.op, lhs.values,
                                                 rhs.values, *root.checkedType)
                                                 .overflow}
          : core::evaluateInteger(root.op, lhs.values, rhs.values,
                                  root.wrapSigned);
  if (lhs.mayBeInvalid || rhs.mayBeInvalid) {
    result.values = core::IntegerRange::full(root.type);
    result.mayBeInvalid = true;
    result.alwaysInvalid = false;
    return result;
  }
  if (root.kind == core::IntegerNodeKind::Operation && operands.size() == 2 &&
      operationDoesNotOverflow(root.op, operands.front(), operands.back(),
                               root.type, state)) {
    // Evaluate modulo the destination width, then discard the impossible
    // overflow alternatives. Nonnegative products/sums cannot become
    // negative on the checked-success edge.
    result = core::evaluateInteger(root.op, lhs.values, rhs.values, true);
    if (root.type.isSigned &&
        (root.op == core::IntegerOp::Add ||
         root.op == core::IntegerOp::Multiply) &&
        !lhs.values.empty() && !rhs.values.empty() &&
        !lhs.values.minimum()->negative() &&
        !rhs.values.minimum()->negative()) {
      const auto zero = core::IntegerRange::singleton(
          core::IntegerValue::ofBits(root.type, 0));
      result.values =
          result.values.satisfying(core::IntegerOp::GreaterEqual, zero);
    }
    result.mayBeInvalid = false;
    result.alwaysInvalid = false;
    result.error = core::IntegerError::None;
  }
  return result;
}

} // namespace weavec::analysis
