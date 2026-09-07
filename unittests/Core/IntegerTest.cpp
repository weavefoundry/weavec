//===- IntegerTest.cpp - Target integers and exhaustive range checks ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Integer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <limits>
#include <utility>

namespace weavec::core {

static constexpr IntegerType U8{.width = 8, .isSigned = false};
static constexpr IntegerType I8{.width = 8, .isSigned = true};
static constexpr IntegerType U32{.width = 32, .isSigned = false};
static constexpr IntegerType I32{.width = 32, .isSigned = true};
static constexpr IntegerType U64{.width = 64, .isSigned = false};
static constexpr IntegerType I64{.width = 64, .isSigned = true};

static IntegerValue integer(IntegerType type, std::int64_t value) {
  return IntegerValue::ofBits(type, static_cast<std::uint64_t>(value));
}

TEST(TargetInteger, TypesAndFullWidthValues) {
  EXPECT_EQ(U64.mask(), UINT64_MAX);
  EXPECT_EQ(I64.signBit(), std::uint64_t{1} << 63U);
  EXPECT_EQ(IntegerType::parse("u64"), U64);
  EXPECT_EQ(IntegerType::parse("b1"), BooleanType);
  EXPECT_FALSE(IntegerType::parse("i0"));
  EXPECT_FALSE(IntegerType::parse("u65"));
  EXPECT_FALSE(IntegerType::parse("i032"));
  EXPECT_FALSE(IntegerType::parse("b8"));
  EXPECT_FALSE((IntegerType{0, true}).valid());
  EXPECT_EQ(IntegerValue::ofBits(U64, UINT64_MAX).toString(),
            "18446744073709551615");
  EXPECT_FALSE(IntegerValue::ofBits(U64, UINT64_MAX).signedValue());
  EXPECT_EQ(integer(I64, INT64_MIN).signedValue(), INT64_MIN);
  EXPECT_EQ(integer(I64, INT64_MIN).toString(), "-9223372036854775808");
  EXPECT_EQ(integer(I8, -1).bits, 255U);
  EXPECT_EQ(integer(I8, -128).magnitude(), 128U);
}

TEST(TargetInteger, ConversionsFollowValueAndDestinationType) {
  EXPECT_EQ(integer(U32, 256).converted(U8).bits, 0U);
  EXPECT_EQ(integer(I8, -1).converted(U32).bits, UINT32_MAX);
  EXPECT_EQ(integer(I8, -128).converted(I64).signedValue(), -128);
  EXPECT_EQ(integer(U8, 255).converted(I32).signedValue(), 255);
  EXPECT_EQ(integer(U32, 255).converted(I8).signedValue(), -1);
  EXPECT_EQ(integer(U32, 256).converted(BooleanType).bits, 1U);
  EXPECT_EQ(integer(I32, 0).converted(BooleanType).bits, 0U);
  EXPECT_EQ(integer(I32, -256).converted(BooleanType).bits, 1U);
}

TEST(TargetInteger, ExactWrappingAndSignedValidity) {
  EXPECT_EQ(
      evaluateInteger(IntegerOp::Add, integer(U8, 255), integer(U8, 1)).value,
      integer(U8, 0));
  EXPECT_EQ(
      evaluateInteger(IntegerOp::Subtract, integer(U64, 0), integer(U64, 1))
          .value,
      IntegerValue::ofBits(U64, UINT64_MAX));
  EXPECT_EQ(
      evaluateInteger(IntegerOp::Multiply, integer(U8, 128), integer(U8, 2))
          .value,
      integer(U8, 0));
  EXPECT_EQ(
      evaluateInteger(IntegerOp::Add, integer(I8, 127), integer(I8, 1)).error,
      IntegerError::SignedOverflow);
  EXPECT_EQ(evaluateInteger(IntegerOp::Subtract, integer(I64, INT64_MIN),
                            integer(I64, 1))
                .error,
            IntegerError::SignedOverflow);
  EXPECT_EQ(evaluateInteger(IntegerOp::Multiply, integer(I64, INT64_MIN),
                            integer(I64, -1))
                .error,
            IntegerError::SignedOverflow);
  EXPECT_EQ(evaluateInteger(IntegerOp::Multiply, integer(I64, INT64_MIN),
                            integer(I64, 1))
                .value,
            integer(I64, INT64_MIN));
  EXPECT_EQ(
      evaluateInteger(IntegerOp::Add, integer(I8, 127), integer(I8, 1), true)
          .value,
      integer(I8, -128));
  EXPECT_EQ(evaluateInteger(IntegerOp::Negate, integer(I64, INT64_MIN),
                            integer(I64, 0))
                .error,
            IntegerError::SignedOverflow);
}

TEST(TargetInteger, DivisionRemainderAndShifts) {
  EXPECT_EQ(
      evaluateInteger(IntegerOp::Divide, integer(I32, -7), integer(I32, 3))
          .value,
      integer(I32, -2));
  EXPECT_EQ(
      evaluateInteger(IntegerOp::Remainder, integer(I32, -7), integer(I32, 3))
          .value,
      integer(I32, -1));
  EXPECT_EQ(evaluateInteger(IntegerOp::Divide, integer(I64, INT64_MIN),
                            integer(I64, -1))
                .error,
            IntegerError::SignedDivisionOverflow);
  EXPECT_EQ(evaluateInteger(IntegerOp::Divide, integer(U32, 7), integer(U32, 0))
                .error,
            IntegerError::DivisionByZero);
  EXPECT_EQ(
      evaluateInteger(IntegerOp::ShiftLeft, integer(U64, 1), integer(I32, 64))
          .error,
      IntegerError::ShiftCount);
  EXPECT_EQ(
      evaluateInteger(IntegerOp::ShiftRight, integer(U64, 1), integer(I32, -1))
          .error,
      IntegerError::ShiftCount);
  EXPECT_EQ(
      evaluateInteger(IntegerOp::ShiftLeft, integer(I32, 1), integer(I32, 31))
          .error,
      IntegerError::SignedLeftShift);
  EXPECT_EQ(
      evaluateInteger(IntegerOp::ShiftRight, integer(I64, -8), integer(I32, 2))
          .value,
      integer(I64, -2));
  EXPECT_EQ(
      evaluateInteger(IntegerOp::ShiftLeft, integer(U64, 1), integer(I32, 63))
          .value,
      IntegerValue::ofBits(U64, std::uint64_t{1} << 63U));
}

TEST(TargetInteger, ExhaustiveSmallWidthConcreteArithmetic) {
  // Independent mathematical oracle: every input/result fits host int here.
  for (const bool isSigned : {false, true}) {
    for (const unsigned width : {2U, 3U, 4U, 5U}) {
      const IntegerType type{.width = width, .isSigned = isSigned};
      const int modulus = static_cast<int>(1U << width);
      const int min = isSigned ? -(modulus / 2) : 0;
      const int max = isSigned ? (modulus / 2) - 1 : modulus - 1;
      for (int a = min; a <= max; ++a) {
        for (int b = min; b <= max; ++b) {
          for (const auto op :
               {IntegerOp::Add, IntegerOp::Subtract, IntegerOp::Multiply}) {
            int mathematical = a * b;
            if (op == IntegerOp::Add)
              mathematical = a + b;
            else if (op == IntegerOp::Subtract)
              mathematical = a - b;
            const auto result =
                evaluateInteger(op, integer(type, a), integer(type, b));
            if (isSigned && (mathematical < min || mathematical > max)) {
              ASSERT_EQ(result.error, IntegerError::SignedOverflow);
            } else {
              const auto bits = static_cast<std::uint64_t>(
                  ((mathematical % modulus) + modulus) % modulus);
              ASSERT_TRUE(result.value);
              ASSERT_EQ(result.value->bits, bits);
            }
          }
          for (const auto op : {IntegerOp::Equal, IntegerOp::NotEqual,
                                IntegerOp::Less, IntegerOp::LessEqual,
                                IntegerOp::Greater, IntegerOp::GreaterEqual}) {
            const bool expected = (op == IntegerOp::Equal && a == b) ||
                                  (op == IntegerOp::NotEqual && a != b) ||
                                  (op == IntegerOp::Less && a < b) ||
                                  (op == IntegerOp::LessEqual && a <= b) ||
                                  (op == IntegerOp::Greater && a > b) ||
                                  (op == IntegerOp::GreaterEqual && a >= b);
            const auto result =
                evaluateInteger(op, integer(type, a), integer(type, b));
            ASSERT_TRUE(result.value);
            ASSERT_EQ(result.value->bits, static_cast<std::uint64_t>(expected));
          }
        }
      }
    }
  }
}

TEST(IntegerRanges, CanonicalParsingAndJoins) {
  const auto a = IntegerRange::between(integer(I8, -4), integer(I8, 9));
  EXPECT_EQ(IntegerRange::parse(a.toString()), a);
  EXPECT_EQ(a.minimum()->signedValue(), -4);
  EXPECT_EQ(a.maximum()->signedValue(), 9);
  EXPECT_FALSE(a.constant());
  EXPECT_TRUE(a.contains(integer(I8, 0)));
  EXPECT_FALSE(a.contains(integer(I8, -5)));
  const auto b = IntegerRange::between(integer(I8, 5), integer(I8, 20));
  EXPECT_EQ(a.intersect(b),
            IntegerRange::between(integer(I8, 5), integer(I8, 9)));
  EXPECT_EQ(a.united(b),
            IntegerRange::between(integer(I8, -4), integer(I8, 20)));
  EXPECT_EQ(a.widened(b),
            IntegerRange::between(integer(I8, -4), integer(I8, 127)));
  EXPECT_EQ(a.widened(a), a);
  EXPECT_TRUE(IntegerRange::fromRanks(U8, {{1, 1}, {3, 3}, {5, 5}}).isFull());
  for (const auto *const text :
       {"u65:0-1", "u8:0-256", "u8:3-2", "u8:1-1,2-2", "u8:1-1,3-3,5-5",
        "u8:01-2", "u8:1-2,", "u8:0-1junk"})
    EXPECT_FALSE(IntegerRange::parse(text)) << text;
}

TEST(IntegerRanges, ConversionWrapAndBoolean) {
  const auto range =
      IntegerRange::between(integer(U32, 250), integer(U32, 260));
  const auto narrow = range.converted(U8);
  EXPECT_EQ(narrow.all(), (std::vector<IntegerInterval>{{0, 4}, {250, 255}}));
  EXPECT_EQ(range.converted(BooleanType).constant(), integer(BooleanType, 1));
  const auto signedRange =
      IntegerRange::between(integer(I8, -4), integer(I8, 4));
  const auto widened = signedRange.converted(U32);
  EXPECT_TRUE(widened.contains(integer(U32, -1)));
  EXPECT_TRUE(widened.contains(integer(U32, 4)));
  EXPECT_FALSE(widened.contains(integer(U32, 5)));
  EXPECT_TRUE(IntegerRange::full(U64).converted(I8).isFull());
  EXPECT_EQ(IntegerRange::full(U64).toString(), "u64:0-18446744073709551615");
  EXPECT_EQ(IntegerRange::parse(IntegerRange::full(U64).toString()),
            IntegerRange::full(U64));
}

TEST(IntegerRanges, ExhaustiveConversionContainsEveryConcreteImage) {
  for (const bool sourceSigned : {false, true}) {
    const IntegerType source{.width = 5, .isSigned = sourceSigned};
    for (std::uint64_t lower = 0; lower < 32; ++lower) {
      for (auto upper = lower; upper < 32; ++upper) {
        const auto range =
            IntegerRange::fromRanks(source, {{.lower = lower, .upper = upper}});
        for (const auto destination :
             {IntegerType{.width = 3, .isSigned = true},
              IntegerType{.width = 3, .isSigned = false}, I8, U8, I64, U64,
              BooleanType}) {
          const auto converted = range.converted(destination);
          for (auto rank = lower; rank <= upper; ++rank) {
            const auto value = IntegerValue::ofBits(source, source.rank(rank));
            const auto mathematical = *value.signedValue();
            std::uint64_t expected =
                static_cast<std::uint64_t>(mathematical) & destination.mask();
            if (destination.isBoolean)
              expected = static_cast<std::uint64_t>(mathematical != 0);
            ASSERT_TRUE(
                converted.contains(IntegerValue::ofBits(destination, expected)))
                << range.toString() << " -> " << destination.toString();
          }
        }
      }
    }
  }
}

// RFC 0017: this oracle deliberately does not call the concrete evaluator,
// rank(), magnitude(), signedValue(), or the range transfer being checked.
// Every mathematical intermediate fits int for the small widths below.
static std::optional<IntegerValue> arithmeticOracle(IntegerOp op, int a, int b,
                                                    IntegerType type,
                                                    bool wrapSigned) {
  const int modulus = static_cast<int>(1U << type.width);
  const int minimum = type.isSigned ? -modulus / 2 : 0;
  const int maximum = type.isSigned ? (modulus / 2) - 1 : modulus - 1;
  const auto bits = [modulus](int value) {
    return static_cast<unsigned>(((value % modulus) + modulus) % modulus);
  };
  const auto boolean = [](bool value) {
    return IntegerValue{.type = BooleanType,
                        .bits = static_cast<std::uint64_t>(value)};
  };
  int result = 0;
  bool checkOverflow = false;
  switch (op) {
  case IntegerOp::Add:
    result = a + b;
    checkOverflow = true;
    break;
  case IntegerOp::Subtract:
    result = a - b;
    checkOverflow = true;
    break;
  case IntegerOp::Multiply:
    result = a * b;
    checkOverflow = true;
    break;
  case IntegerOp::Divide:
  case IntegerOp::Remainder:
    if (b == 0 || (type.isSigned && a == minimum && b == -1))
      return std::nullopt;
    result = op == IntegerOp::Divide ? a / b : a % b;
    break;
  case IntegerOp::ShiftLeft:
  case IntegerOp::ShiftRight: {
    if (b < 0 || std::cmp_greater_equal(b, type.width))
      return std::nullopt;
    const int factor = static_cast<int>(1U << static_cast<unsigned>(b));
    if (op == IntegerOp::ShiftLeft) {
      result = a * factor;
      // -fwrapv does not define negative or overflowing signed left shifts.
      if (type.isSigned && (a < 0 || result > maximum))
        return std::nullopt;
    } else {
      // Supported targets use arithmetic right shift: floor(a / 2**b).
      result = a >= 0 ? a / factor : -((-a + factor - 1) / factor);
    }
    break;
  }
  case IntegerOp::BitAnd:
    result = static_cast<int>(bits(a) & bits(b));
    break;
  case IntegerOp::BitOr:
    result = static_cast<int>(bits(a) | bits(b));
    break;
  case IntegerOp::BitXor:
    result = static_cast<int>(bits(a) ^ bits(b));
    break;
  case IntegerOp::Negate:
    result = -a;
    checkOverflow = true;
    break;
  case IntegerOp::Complement:
    result = modulus - 1 - static_cast<int>(bits(a));
    break;
  case IntegerOp::LogicalNot:
    return boolean(a == 0);
  case IntegerOp::Equal:
    return boolean(a == b);
  case IntegerOp::NotEqual:
    return boolean(a != b);
  case IntegerOp::Less:
    return boolean(a < b);
  case IntegerOp::LessEqual:
    return boolean(a <= b);
  case IntegerOp::Greater:
    return boolean(a > b);
  case IntegerOp::GreaterEqual:
    return boolean(a >= b);
  case IntegerOp::Minimum:
    result = std::min(a, b);
    break;
  case IntegerOp::Maximum:
    result = std::max(a, b);
    break;
  }
  if (checkOverflow && type.isSigned && !wrapSigned &&
      (result < minimum || result > maximum))
    return std::nullopt;
  return IntegerValue{.type = type,
                      .bits = static_cast<std::uint64_t>(bits(result))};
}

TEST(IntegerRanges, AbstractArithmeticContainsIndependentConcreteImages) {
  constexpr std::array Operations{
      IntegerOp::Add,          IntegerOp::Subtract,  IntegerOp::Multiply,
      IntegerOp::Divide,       IntegerOp::Remainder, IntegerOp::ShiftLeft,
      IntegerOp::ShiftRight,   IntegerOp::BitAnd,    IntegerOp::BitOr,
      IntegerOp::BitXor,       IntegerOp::Negate,    IntegerOp::Complement,
      IntegerOp::LogicalNot,   IntegerOp::Equal,     IntegerOp::NotEqual,
      IntegerOp::Less,         IntegerOp::LessEqual, IntegerOp::Greater,
      IntegerOp::GreaterEqual, IntegerOp::Minimum,   IntegerOp::Maximum};
  bool sawMixedValidity = false;
  for (const bool signedType : {false, true}) {
    for (const unsigned width : {3U, 5U}) {
      const IntegerType type{.width = width, .isSigned = signedType};
      const int count = static_cast<int>(1U << width);
      const int minimum = signedType ? -count / 2 : 0;
      std::vector<IntegerRange> ranges;
      for (int lower = 0; lower < count; lower += std::max(1, count / 5))
        for (int upper = lower; upper < count; upper += std::max(1, count / 3))
          ranges.push_back(IntegerRange::between(
              integer(type, minimum + lower), integer(type, minimum + upper)));
      // The five-bit full range exercises transfer beyond smallValues' cap.
      ranges.push_back(IntegerRange::full(type));
      ranges.push_back(IntegerRange::fromRanks(
          type, {{.lower = 0, .upper = 1},
                 {.lower = static_cast<std::uint64_t>(count - 2),
                  .upper = static_cast<std::uint64_t>(count - 1)}}));
      for (const auto &lhs : ranges)
        for (const auto &rhs : ranges)
          for (const auto op : Operations)
            for (const bool wrapSigned : {false, true}) {
              const auto actual = evaluateInteger(op, lhs, rhs, wrapSigned);
              bool valid = false;
              bool invalid = false;
              for (int a = minimum; a < minimum + count; ++a) {
                if (!lhs.contains(integer(type, a)))
                  continue;
                for (int b = minimum; b < minimum + count; ++b) {
                  if (!rhs.contains(integer(type, b)))
                    continue;
                  const auto expected =
                      arithmeticOracle(op, a, b, type, wrapSigned);
                  valid |= expected.has_value();
                  invalid |= !expected;
                  if (expected)
                    ASSERT_TRUE(actual.values.contains(*expected))
                        << toString(op) << ' ' << lhs.toString() << ' '
                        << rhs.toString() << " wrap=" << wrapSigned
                        << " misses bits=" << expected->bits;
                }
              }
              ASSERT_FALSE(actual.alwaysInvalid && valid);
              ASSERT_FALSE(invalid && !actual.mayBeInvalid)
                  << toString(op) << ' ' << lhs.toString() << ' '
                  << rhs.toString();
              if (invalid) {
                // Invalid arithmetic must not leave a fact usable to prune an
                // edge.
                EXPECT_TRUE(actual.values.isFull());
                sawMixedValidity |= valid;
              }
            }
    }
  }
  EXPECT_TRUE(sawMixedValidity);
}

TEST(IntegerRanges, RefinementContainsEverySatisfyingValue) {
  const IntegerType type{.width = 4, .isSigned = true};
  for (std::uint64_t lower = 0; lower < 16; ++lower) {
    for (auto upper = lower; upper < 16; ++upper) {
      const auto lhs =
          IntegerRange::fromRanks(type, {{.lower = lower, .upper = upper}});
      for (std::uint64_t bound = 0; bound < 16; ++bound) {
        const auto rhs = IntegerRange::singleton(
            IntegerValue::ofBits(type, type.rank(bound)));
        for (const auto op : {IntegerOp::Equal, IntegerOp::NotEqual,
                              IntegerOp::Less, IntegerOp::LessEqual,
                              IntegerOp::Greater, IntegerOp::GreaterEqual}) {
          const auto narrowed = lhs.satisfying(op, rhs);
          for (auto rank = lower; rank <= upper; ++rank) {
            const auto value = IntegerValue::ofBits(type, type.rank(rank));
            const bool expected =
                evaluateInteger(op, value, *rhs.constant()).value->bits != 0;
            ASSERT_EQ(narrowed.contains(value), expected);
          }
        }
      }
    }
  }
}

TEST(IntegerRanges, ModularScalingPreservesSeparatedConvertedIntervals) {
  const IntegerType source{.width = 32, .isSigned = true};
  const IntegerType size{.width = 64, .isSigned = false};
  const auto nonzero = IntegerRange::full(source).satisfying(
      IntegerOp::NotEqual,
      IntegerRange::singleton(IntegerValue::ofBits(source, 0)));
  const auto scaled =
      evaluateInteger(IntegerOp::Multiply, nonzero.converted(size),
                      IntegerRange::singleton(IntegerValue::ofBits(size, 8)));
  EXPECT_FALSE(scaled.values.contains(IntegerValue::ofBits(size, 0)));
  EXPECT_TRUE(scaled.values.contains(IntegerValue::ofBits(size, 8)));
  EXPECT_TRUE(
      scaled.values.contains(IntegerValue::ofBits(size, UINT64_MAX - 7)));
  for (unsigned width = 5; width <= 7; ++width) {
    const IntegerType type{.width = width, .isSigned = false};
    for (unsigned factor = 0; factor <= type.mask(); ++factor) {
      const auto rhs =
          IntegerRange::singleton(IntegerValue::ofBits(type, factor));
      for (unsigned begin = 0; begin <= type.mask(); begin += 13) {
        const auto lhs = IntegerRange::fromRanks(
            type, {{.lower = begin, .upper = type.mask()}});
        const auto product = evaluateInteger(IntegerOp::Multiply, lhs, rhs);
        const auto reversed = evaluateInteger(IntegerOp::Multiply, rhs, lhs);
        ASSERT_EQ(product.values, reversed.values);
        for (unsigned value = begin; value <= type.mask(); ++value)
          ASSERT_TRUE(product.values.contains(IntegerValue::ofBits(
              type, static_cast<std::uint64_t>(value) * factor)));
      }
    }
  }
}

TEST(TargetInteger, CheckedArithmeticUsesOriginalTypesAndOutputWidth) {
  const auto max = IntegerValue::ofBits(U64, UINT64_MAX);
  ASSERT_TRUE(evaluateCheckedInteger(IntegerOp::Multiply, max, max, U64));
  EXPECT_EQ(
      evaluateCheckedInteger(IntegerOp::Multiply, max, max, U64)->value.bits,
      1U);
  EXPECT_TRUE(
      evaluateCheckedInteger(IntegerOp::Multiply, max, max, U64)->overflow);
  EXPECT_FALSE(
      evaluateCheckedInteger(IntegerOp::Add, max, integer(I64, -1), U64)
          ->overflow);
  EXPECT_EQ(evaluateCheckedInteger(IntegerOp::Add, max, integer(I64, -1), U64)
                ->value.bits,
            UINT64_MAX - 1);
  EXPECT_FALSE(
      evaluateCheckedInteger(IntegerOp::Subtract, max, max, I8)->overflow);
  EXPECT_TRUE(
      evaluateCheckedInteger(IntegerOp::Subtract, integer(U8, 0), max, I64)
          ->overflow);
  EXPECT_TRUE(evaluateCheckedInteger(IntegerOp::Add, integer(I8, -1),
                                     integer(I8, 0), U64)
                  ->overflow);
  EXPECT_FALSE(evaluateCheckedInteger(IntegerOp::Multiply,
                                      integer(I64, INT64_MIN), integer(I64, 1),
                                      I64)
                   ->overflow);
  EXPECT_TRUE(evaluateCheckedInteger(IntegerOp::Multiply,
                                     integer(I64, INT64_MIN), integer(I64, -1),
                                     I64)
                  ->overflow);
  for (const auto aType : {IntegerType{.width = 5, .isSigned = true},
                           IntegerType{.width = 5, .isSigned = false}})
    for (const auto bType : {IntegerType{.width = 5, .isSigned = true},
                             IntegerType{.width = 5, .isSigned = false}})
      for (const auto output : {IntegerType{.width = 4, .isSigned = true},
                                IntegerType{.width = 4, .isSigned = false},
                                IntegerType{.width = 6, .isSigned = true}})
        for (unsigned a = 0; a < 32; ++a)
          for (unsigned b = 0; b < 32; ++b)
            for (const auto op :
                 {IntegerOp::Add, IntegerOp::Subtract, IntegerOp::Multiply}) {
              const auto x = IntegerValue::ofBits(aType, a);
              const auto y = IntegerValue::ofBits(bType, b);
              const auto left = *x.signedValue();
              const auto right = *y.signedValue();
              auto exact = left * right;
              if (op == IntegerOp::Add)
                exact = left + right;
              else if (op == IntegerOp::Subtract)
                exact = left - right;
              const auto actual = evaluateCheckedInteger(op, x, y, output);
              ASSERT_TRUE(actual);
              const auto lo = output.isSigned
                                  ? -static_cast<std::int64_t>(output.signBit())
                                  : 0;
              const auto hi = static_cast<std::int64_t>(
                  output.isSigned ? output.signBit() - 1 : output.mask());
              EXPECT_EQ(actual->overflow, exact < lo || exact > hi);
              EXPECT_EQ(actual->value.bits,
                        static_cast<std::uint64_t>(exact) & output.mask());
            }
}

TEST(TargetInteger, WrapSignedDoesNotRelaxShiftOrDivisionValidity) {
  for (const bool wrapSigned : {false, true}) {
    for (const auto operands : {std::pair{-1, 0}, std::pair{1, 7},
                                std::pair{64, 1}, std::pair{-128, 1}}) {
      const auto result =
          evaluateInteger(IntegerOp::ShiftLeft, integer(I8, operands.first),
                          integer(I32, operands.second), wrapSigned);
      EXPECT_FALSE(result.value);
      EXPECT_EQ(result.error, IntegerError::SignedLeftShift);
    }
    for (const auto op : {IntegerOp::ShiftLeft, IntegerOp::ShiftRight}) {
      for (const auto count : {integer(I32, -1), integer(I32, 8),
                               IntegerValue::ofBits(U64, UINT64_MAX)}) {
        const auto result =
            evaluateInteger(op, integer(I8, 1), count, wrapSigned);
        EXPECT_FALSE(result.value);
        EXPECT_EQ(result.error, IntegerError::ShiftCount);
      }
    }
    for (const auto op : {IntegerOp::Divide, IntegerOp::Remainder})
      EXPECT_EQ(
          evaluateInteger(op, integer(I8, -128), integer(I8, -1), wrapSigned)
              .error,
          IntegerError::SignedDivisionOverflow);
    const auto shifted =
        evaluateInteger(IntegerOp::ShiftLeft,
                        IntegerRange::between(integer(I8, 0), integer(I8, 100)),
                        IntegerRange::singleton(integer(I32, 1)), wrapSigned);
    EXPECT_TRUE(shifted.mayBeInvalid);
    EXPECT_FALSE(shifted.alwaysInvalid);
    EXPECT_TRUE(shifted.values.isFull());
    const auto negative = evaluateInteger(
        IntegerOp::ShiftLeft, IntegerRange::singleton(integer(I8, -1)),
        IntegerRange::singleton(integer(I32, 0)), wrapSigned);
    EXPECT_TRUE(negative.alwaysInvalid);
    EXPECT_EQ(negative.error, IntegerError::SignedLeftShift);
    const auto valid = evaluateInteger(IntegerOp::ShiftLeft, integer(I8, 63),
                                       integer(I32, 1), wrapSigned);
    EXPECT_EQ(valid.value, integer(I8, 126));
    EXPECT_EQ(evaluateInteger(IntegerOp::ShiftRight, integer(I8, -3),
                              integer(I32, 1), wrapSigned)
                  .value,
              integer(I8, -2));
  }
}

TEST(IntegerRanges, CheckedArithmeticContainsIndependentMathematicalResults) {
  for (const bool aSigned : {false, true})
    for (const bool bSigned : {false, true}) {
      const IntegerType aType{.width = 5, .isSigned = aSigned};
      const IntegerType bType{.width = 5, .isSigned = bSigned};
      const int aMin = aSigned ? -16 : 0;
      const int bMin = bSigned ? -16 : 0;
      for (const auto aRange : {IntegerInterval{.lower = 0, .upper = 31},
                                IntegerInterval{.lower = 0, .upper = 18},
                                IntegerInterval{.lower = 15, .upper = 31}})
        for (const auto bRange : {IntegerInterval{.lower = 0, .upper = 31},
                                  IntegerInterval{.lower = 0, .upper = 18},
                                  IntegerInterval{.lower = 15, .upper = 31}}) {
          const auto lhs = IntegerRange::fromRanks(aType, {aRange});
          const auto rhs = IntegerRange::fromRanks(bType, {bRange});
          for (const auto output :
               {IntegerType{.width = 4, .isSigned = true},
                IntegerType{.width = 4, .isSigned = false},
                IntegerType{.width = 6, .isSigned = true}, BooleanType})
            for (const auto op :
                 {IntegerOp::Add, IntegerOp::Subtract, IntegerOp::Multiply}) {
              const auto actual = evaluateCheckedInteger(op, lhs, rhs, output);
              const int modulus = static_cast<int>(1U << output.width);
              const int lo = output.isSigned ? -modulus / 2 : 0;
              const int hi = output.isSigned ? (modulus / 2) - 1 : modulus - 1;
              for (int a = aMin; a < aMin + 32; ++a) {
                if (!lhs.contains(integer(aType, a)))
                  continue;
                for (int b = bMin; b < bMin + 32; ++b) {
                  if (!rhs.contains(integer(bType, b)))
                    continue;
                  int exact = a * b;
                  if (op == IntegerOp::Add)
                    exact = a + b;
                  else if (op == IntegerOp::Subtract)
                    exact = a - b;
                  const int bits =
                      output.isBoolean
                          ? static_cast<int>(exact != 0)
                          : ((exact % modulus) + modulus) % modulus;
                  ASSERT_TRUE(actual.values.contains(
                      IntegerValue{output, static_cast<std::uint64_t>(bits)}));
                  ASSERT_TRUE(actual.overflow.contains(IntegerValue{
                      BooleanType,
                      static_cast<std::uint64_t>(exact < lo || exact > hi)}))
                      << toString(op) << ' ' << lhs.toString() << ' '
                      << rhs.toString() << " -> " << output.toString();
                }
              }
            }
        }
    }
}

TEST(TargetInteger, MalformedAndOversizedTypesDoNotBecomeSupportedWidths) {
  for (const auto *const text :
       {"u4294967297", "i18446744073709551615", "u18446744073709551616", "u+8",
        "i-1", "b0", "b64"})
    EXPECT_FALSE(IntegerType::parse(text)) << text;
  for (const auto type :
       {IntegerType{.width = 0, .isSigned = false},
        IntegerType{.width = 65, .isSigned = true},
        IntegerType{.width = UINT_MAX, .isSigned = false},
        IntegerType{.width = 1, .isSigned = true, .isBoolean = true}}) {
    EXPECT_FALSE(type.valid());
    EXPECT_EQ(type.mask(), 0U);
    EXPECT_EQ(type.signBit(), 0U);
    EXPECT_EQ(evaluateInteger(IntegerOp::Add, IntegerValue{type, 0},
                              IntegerValue{type, 0})
                  .error,
              IntegerError::IncompatibleTypes);
    EXPECT_FALSE(evaluateCheckedInteger(IntegerOp::Add, integer(U8, 0),
                                        integer(U8, 0), type));
  }
}

} // namespace weavec::core
