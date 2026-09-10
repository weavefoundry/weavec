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
  if (state.safety && !type.isSigned && lhs.type() == type &&
      rhs.type() == type) {
    if (op == core::IntegerOp::Subtract && lhs.inputKey() && rhs.inputKey() &&
        !numericExpressions.contains(*lhs.inputKey()) &&
        !numericExpressions.contains(*rhs.inputKey()) &&
        checkedAtMost(core::Affine::ofPlace(*rhs.inputKey()),
                      core::Affine::ofPlace(*lhs.inputKey()), state))
      return true;
    if (op == core::IntegerOp::Subtract && rhs.constantValue()) {
      const auto &root = lhs.all().back();
      const auto parts = lhs.operands();
      const auto count = rhs.constantValue()->signedValue();
      if (root.kind == core::IntegerNodeKind::Operation &&
          root.op == core::IntegerOp::Subtract && parts.size() == 2 &&
          parts.front().inputKey() && parts.back().inputKey() && count &&
          *count >= 0 &&
          !numericExpressions.contains(*parts.front().inputKey()) &&
          !numericExpressions.contains(*parts.back().inputKey()) &&
          checkedAtMost(
              core::Affine::ofPlace(*parts.back().inputKey(), 1, *count),
              core::Affine::ofPlace(*parts.front().inputKey()), state))
        return true;
    }
    if (op == core::IntegerOp::Add)
      if (const auto sum = NumericExpression::operation(op, lhs, rhs))
        if (const auto bound = checkedTraversalSum(*sum, state);
            bound && bound->place) {
          const auto values = integerRangeAt(*bound->place, type, state);
          if (!values.empty() && !values.maximum()->negative() &&
              (bound->constant <= 0 ||
               (static_cast<std::uint64_t>(bound->constant) <= type.mask() &&
                values.maximum()->bits <=
                    type.mask() - static_cast<std::uint64_t>(bound->constant))))
            return true;
        }
  }
  const auto overflowExpression =
      NumericExpression::overflow(op, lhs, rhs, type);
  // RFC 0019: `a <= MAX - b - k` proves the nonnegative sum a+b+k.
  // Match the evaluated expressions, preserving the target type and every
  // subtraction's no-underflow premise. This also handles a trailing +1
  // for a buffer's terminator without interpreting a wrapped sum as bytes.
  std::vector<NumericExpression> summands;
  std::uint64_t constant = 0;
  bool simpleSum = op == core::IntegerOp::Add && !type.isSigned;
  const std::function<void(const NumericExpression &)> flatten =
      [&](const NumericExpression &value) {
        if (!simpleSum || value.type() != type) {
          simpleSum = false;
          return;
        }
        if (const auto exact = value.constantValue()) {
          if (__builtin_add_overflow(constant, exact->bits, &constant))
            simpleSum = false;
          return;
        }
        const auto &node = value.all().back();
        if (node.kind == core::IntegerNodeKind::Operation &&
            node.op == core::IntegerOp::Add) {
          for (const auto &operand : value.operands())
            flatten(operand);
        } else {
          summands.push_back(value);
        }
      };
  if (simpleSum) {
    flatten(lhs);
    flatten(rhs);
    simpleSum &= summands.size() == 2;
  }
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
    if (simpleSum && !predicate.range) {
      auto smaller = predicate.lhs;
      auto room = predicate.rhs;
      auto relation = predicate.op;
      if (relation == core::IntegerOp::Greater ||
          relation == core::IntegerOp::GreaterEqual) {
        std::swap(smaller, room);
        relation = core::reverseComparison(relation);
      }
      const auto parts = room.operands();
      if ((relation == core::IntegerOp::Less ||
           relation == core::IntegerOp::LessEqual) &&
          smaller.type() == type && room.type() == type && parts.size() == 2 &&
          room.all().back().kind == core::IntegerNodeKind::Operation &&
          room.all().back().op == core::IntegerOp::Subtract) {
        const auto maximum = parts.front().constantValue();
        const auto subtracted = parts.back().evaluate(read);
        const bool matched =
            (smaller == summands.front() && parts.back() == summands.back()) ||
            (smaller == summands.back() && parts.back() == summands.front());
        // RFC 0021: count <= length-index also bounds index+count by
        // length, provided the remaining-length subtraction cannot wrap.
        const auto length = parts.front().inputKey();
        const auto index = parts.back().inputKey();
        if (state.safety && matched && length && index &&
            parts.front().type() == type && parts.back().type() == type &&
            checkedAtMost(core::Affine::ofPlace(*index),
                          core::Affine::ofPlace(*length), state) &&
            constant <= (relation == core::IntegerOp::Less ? 1U : 0U))
          return true;
        if (maximum && matched && !subtracted.mayBeInvalid &&
            !subtracted.values.empty() &&
            subtracted.values.maximum()->bits <= maximum->bits) {
          const auto slack = type.mask() - maximum->bits;
          if (constant <= slack ||
              (relation == core::IntegerOp::Less && constant - slack == 1))
            return true;
        }
      }
    }
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
    if ((root.op == core::IntegerOp::Add ||
         root.op == core::IntegerOp::Multiply) &&
        !lhs.values.empty() && !rhs.values.empty() &&
        !lhs.values.minimum()->negative() &&
        !rhs.values.minimum()->negative()) {
      const auto minimum = core::evaluateCheckedInteger(
          root.op, core::IntegerRange::singleton(*lhs.values.minimum()),
          core::IntegerRange::singleton(*rhs.values.minimum()), root.type);
      if (const auto overflow = minimum.overflow.constant();
          overflow && overflow->bits == 0) {
        const auto floor = core::evaluateInteger(root.op, *lhs.values.minimum(),
                                                 *rhs.values.minimum());
        if (floor.value)
          result.values = result.values.satisfying(
              core::IntegerOp::GreaterEqual,
              core::IntegerRange::singleton(*floor.value));
      }
    }
    result.mayBeInvalid = false;
    result.alwaysInvalid = false;
    result.error = core::IntegerError::None;
  }
  return result;
}

} // namespace weavec::analysis
