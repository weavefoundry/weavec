//===- DataflowPointerOperations.cpp - Same-array operations (RFC 0021) --===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AffineSupport.h"
#include "Dataflow.h"
#include "IntegerSupport.h"
#include "weavec/Core/Traversal.h"

using namespace clang;

namespace weavec::analysis {

std::optional<core::IntegerValue>
FunctionDataflow::checkedPointerOperation(const BinaryOperator &expr,
                                          const core::AnalysisState &state) {
  const auto range = checkedPointerRange(expr, state);
  return range ? range->constant() : std::nullopt;
}

std::optional<core::IntegerRange>
FunctionDataflow::checkedPointerRange(const BinaryOperator &expr,
                                      const core::AnalysisState &state) {
  if (!state.safety || (expr.getOpcode() != BO_Sub && !expr.isRelationalOp()))
    return std::nullopt;
  const auto leftType = expr.getLHS()->getType();
  const auto rightType = expr.getRHS()->getType();
  if (!leftType->isPointerType() || !rightType->isPointerType() ||
      !ASTContext::hasSameUnqualifiedType(leftType->getPointeeType(),
                                          rightType->getPointeeType()))
    return std::nullopt;
  const auto lhs = checkedMemory(*expr.getLHS(), {}, {}, state);
  const auto rhs = checkedMemory(*expr.getRHS(), {}, {}, state);
  if (!lhs || !rhs || lhs->storage != rhs->storage ||
      !checkedValid(*lhs, state) || !checkedValid(*rhs, state) ||
      !lhs->extent || !rhs->extent ||
      !checkedInterval(lhs->begin, lhs->end, *lhs->extent, state) ||
      !checkedInterval(rhs->begin, rhs->end, *rhs->extent, state))
    return std::nullopt;
  const auto left = foldAffine(lhs->begin, state);
  const auto right = foldAffine(rhs->begin, state);
  const auto unit = byteSizeOf(leftType->getPointeeType(), context);
  const auto type = integerTypeOf(expr.getType(), context);
  if (!unit || *unit <= 0 || !type)
    return std::nullopt;
  if (expr.getOpcode() != BO_Sub) {
    if (!left.isConstant() || !right.isConstant())
      return core::IntegerRange::between(core::IntegerValue::ofBits(*type, 0),
                                         core::IntegerValue::ofBits(*type, 1));
    bool result = false;
    switch (expr.getOpcode()) {
    case BO_LT:
      result = left.constant < right.constant;
      break;
    case BO_LE:
      result = left.constant <= right.constant;
      break;
    case BO_GT:
      result = left.constant > right.constant;
      break;
    case BO_GE:
      result = left.constant >= right.constant;
      break;
    default:
      return std::nullopt;
    }
    return core::IntegerRange::singleton(
        core::IntegerValue::ofBits(*type, result ? 1 : 0));
  }
  if (left.isConstant() && right.isConstant()) {
    const auto exact =
        core::pointerDifference(left.constant, right.constant, *unit, *type);
    return exact ? std::optional(core::IntegerRange::singleton(*exact))
                 : std::nullopt;
  }
  // Byte coordinates have mathematical integer values. For larger elements,
  // require an explicit multiple of the element size on both operands.
  if ((left.place && left.scale % *unit != 0) || left.constant % *unit != 0 ||
      (right.place && right.scale % *unit != 0) || right.constant % *unit != 0)
    return std::nullopt;
  const auto bound = [&](const core::Affine &value,
                         bool upper) -> std::optional<std::int64_t> {
    if (!value.place)
      return value.constant;
    const auto limits = integerBounds(*value.place, state);
    auto result = (upper == (value.scale >= 0)) ? limits.second : limits.first;
    if (!result || __builtin_mul_overflow(*result, value.scale, &*result) ||
        __builtin_add_overflow(*result, value.constant, &*result))
      return std::nullopt;
    return result;
  };
  auto lowerLeft = bound(left, false);
  auto upperLeft = bound(left, true);
  auto lowerRight = bound(right, false);
  auto upperRight = bound(right, true);
  std::optional<std::int64_t> lower;
  std::optional<std::int64_t> upper;
  std::int64_t value = 0;
  if (lowerLeft && upperRight &&
      !__builtin_sub_overflow(*lowerLeft, *upperRight, &value))
    lower = value;
  if (upperLeft && lowerRight &&
      !__builtin_sub_overflow(*upperLeft, *lowerRight, &value))
    upper = value;
  if (left.scale == right.scale && left.scale > 0) {
    const auto relations = checkedRelations(state);
    const auto forward = relations.bound(left.place, right.place);
    const auto reverse = relations.bound(right.place, left.place);
    inferred.checked.limited |= relations.limited();
    if (relations.limited())
      inferred.incomplete.insert("traversal relational limit reached");
    std::int64_t shift = 0;
    if (!__builtin_sub_overflow(left.constant, right.constant, &shift)) {
      if (forward && !__builtin_mul_overflow(*forward, left.scale, &value) &&
          !__builtin_add_overflow(value, shift, &value))
        upper = upper ? std::min(*upper, value) : value;
      if (reverse && !__builtin_mul_overflow(*reverse, -left.scale, &value) &&
          !__builtin_add_overflow(value, shift, &value))
        lower = lower ? std::max(*lower, value) : value;
    }
  }
  if (!lower || !upper || *lower > *upper)
    return std::nullopt;
  const auto first = core::pointerDifference(*lower, 0, *unit, *type);
  const auto last = core::pointerDifference(*upper, 0, *unit, *type);
  return first && last
             ? std::optional(core::IntegerRange::between(*first, *last))
             : std::nullopt;
}

} // namespace weavec::analysis
