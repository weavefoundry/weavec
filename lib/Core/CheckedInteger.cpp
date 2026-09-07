//===- CheckedInteger.cpp - Infinite-precision overflow tests (RFC 0017) --===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Integer.h"

#include <algorithm>

namespace weavec::core {

namespace {
struct CheckedMagnitude {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  bool negative = false;
};
} // namespace

static CheckedMagnitude checkedMagnitude(IntegerOp op, IntegerValue lhs,
                                         IntegerValue rhs) {
  const auto a = lhs.magnitude();
  const auto b = rhs.magnitude();
  if (op == IntegerOp::Multiply) {
    constexpr std::uint64_t HalfMask = UINT32_MAX;
    const auto aLow = a & HalfMask;
    const auto aHigh = a >> 32U;
    const auto bLow = b & HalfMask;
    const auto bHigh = b >> 32U;
    const auto first = aLow * bLow;
    const auto middle = (aHigh * bLow) + (first >> 32U);
    const auto carry = middle >> 32U;
    const auto second = (middle & HalfMask) + (aLow * bHigh);
    return {.high = (aHigh * bHigh) + carry + (second >> 32U),
            .low = (second << 32U) | (first & HalfMask),
            .negative = a != 0 && b != 0 && lhs.negative() != rhs.negative()};
  }
  const bool negativeA = lhs.negative();
  const bool negativeB =
      rhs.negative() != (op == IntegerOp::Subtract && b != 0);
  if (negativeA == negativeB) {
    const auto low = a + b;
    return {.high = low < a ? 1U : 0U, .low = low, .negative = negativeA};
  }
  return {.high = 0,
          .low = a >= b ? a - b : b - a,
          .negative = a != b && (a > b ? negativeA : negativeB)};
}

/// Below, inside or above the destination's mathematical value interval.
static int relativeToType(const CheckedMagnitude &value, IntegerType type) {
  if (value.negative) {
    if (!type.isSigned || value.high != 0 || value.low > type.signBit())
      return -1;
  } else {
    const auto maximum = type.isSigned ? type.signBit() - 1 : type.mask();
    if (value.high != 0 || value.low > maximum)
      return 1;
  }
  return 0;
}

std::optional<CheckedIntegerValue>
evaluateCheckedInteger(IntegerOp op, IntegerValue lhs, IntegerValue rhs,
                       IntegerType destination) {
  if ((op != IntegerOp::Add && op != IntegerOp::Subtract &&
       op != IntegerOp::Multiply) ||
      !lhs.type.valid() || !rhs.type.valid() || !destination.valid())
    return std::nullopt;
  const auto magnitude = checkedMagnitude(op, lhs, rhs);
  const auto bits =
      magnitude.negative ? std::uint64_t{0} - magnitude.low : magnitude.low;
  const auto value = IntegerValue::ofBits(
      destination, destination.isBoolean
                       ? static_cast<std::uint64_t>(magnitude.high != 0 ||
                                                    magnitude.low != 0)
                       : bits);
  return CheckedIntegerValue{
      .value = value, .overflow = relativeToType(magnitude, destination) != 0};
}

CheckedIntegerRange evaluateCheckedInteger(IntegerOp op,
                                           const IntegerRange &lhs,
                                           const IntegerRange &rhs,
                                           IntegerType destination) {
  CheckedIntegerRange result{.values = IntegerRange::full(destination),
                             .overflow = IntegerRange::full(BooleanType)};
  if (lhs.empty() || rhs.empty())
    return {.values = IntegerRange(destination),
            .overflow = IntegerRange(BooleanType)};
  if (const auto a = lhs.constant())
    if (const auto b = rhs.constant())
      if (const auto exact = evaluateCheckedInteger(op, *a, *b, destination))
        return {.values = IntegerRange::singleton(exact->value),
                .overflow = IntegerRange::singleton(IntegerValue::ofBits(
                    BooleanType, static_cast<std::uint64_t>(exact->overflow)))};
  if (op != IntegerOp::Add && op != IntegerOp::Subtract &&
      op != IntegerOp::Multiply)
    return result;
  if (!destination.isBoolean) {
    const auto wrapped = evaluateInteger(op, lhs.converted(destination),
                                         rhs.converted(destination), true);
    if (!wrapped.mayBeInvalid)
      result.values = wrapped.values;
  }
  bool allInside = true;
  bool allOutside = true;
  for (const auto &a : lhs.all())
    for (const auto &b : rhs.all()) {
      bool below = true;
      bool above = true;
      for (const auto x : {a.lower, a.upper})
        for (const auto y : {b.lower, b.upper}) {
          const auto value = checkedMagnitude(
              op, IntegerValue::ofBits(lhs.type, lhs.type.rank(x)),
              IntegerValue::ofBits(rhs.type, rhs.type.rank(y)));
          const auto side = relativeToType(value, destination);
          allInside &= side == 0;
          below &= side < 0;
          above &= side > 0;
        }
      allOutside &= below || above;
    }
  if (allInside || allOutside)
    result.overflow = IntegerRange::singleton(IntegerValue::ofBits(
        BooleanType, static_cast<std::uint64_t>(allOutside)));
  return result;
}

} // namespace weavec::core
