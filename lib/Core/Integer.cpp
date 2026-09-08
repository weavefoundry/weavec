//===- Integer.cpp - Target integer values and bounded ranges -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Integer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <span>
#include <utility>

namespace weavec::core {

bool IntegerType::valid() const noexcept {
  return width >= 1 && width <= 64 && (!isBoolean || (width == 1 && !isSigned));
}

std::uint64_t IntegerType::mask() const noexcept {
  if (!valid())
    return 0;
  return width == 64 ? UINT64_MAX : (std::uint64_t{1} << width) - 1;
}

std::uint64_t IntegerType::signBit() const noexcept {
  return valid() && isSigned ? std::uint64_t{1} << (width - 1) : 0;
}

std::uint64_t IntegerType::rank(std::uint64_t bits) const noexcept {
  return (bits & mask()) ^ signBit();
}

std::string IntegerType::toString() const {
  return isBoolean ? "b1"
                   : std::string(isSigned ? "i" : "u") + std::to_string(width);
}

static std::optional<std::uint64_t> unsignedNumber(std::string_view text) {
  if (text.empty() || (text.size() > 1 && text.front() == '0'))
    return std::nullopt;
  std::uint64_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::optional<IntegerType> IntegerType::parse(std::string_view text) {
  if (text == "b1")
    return BooleanType;
  if (text.size() < 2 || (text.front() != 'i' && text.front() != 'u'))
    return std::nullopt;
  const auto width = unsignedNumber(text.substr(1));
  if (!width || *width < 1 || *width > 64)
    return std::nullopt;
  return IntegerType{.width = static_cast<unsigned>(*width),
                     .isSigned = text.front() == 'i',
                     .isBoolean = false};
}

IntegerValue IntegerValue::ofBits(IntegerType type,
                                  std::uint64_t bits) noexcept {
  return {.type = type,
          .bits = type.isBoolean ? static_cast<std::uint64_t>(bits != 0)
                                 : bits & type.mask()};
}

bool IntegerValue::negative() const noexcept {
  return (bits & type.signBit()) != 0;
}

std::uint64_t IntegerValue::magnitude() const noexcept {
  return negative() ? ((~bits + 1) & type.mask()) : bits;
}

std::optional<std::int64_t> IntegerValue::signedValue() const noexcept {
  if (!negative()) {
    if (bits > static_cast<std::uint64_t>(INT64_MAX))
      return std::nullopt;
    return static_cast<std::int64_t>(bits);
  }
  const std::uint64_t mag = magnitude();
  if (mag == (std::uint64_t{1} << 63U))
    return INT64_MIN;
  return -static_cast<std::int64_t>(mag);
}

IntegerValue IntegerValue::converted(IntegerType destination) const noexcept {
  if (destination.isBoolean)
    return ofBits(destination, static_cast<std::uint64_t>(bits != 0));
  const std::uint64_t extended = negative() ? bits | ~type.mask() : bits;
  return ofBits(destination, extended);
}

std::string IntegerValue::toString() const {
  return (negative() ? "-" : "") + std::to_string(magnitude());
}

static constexpr std::array<std::string_view, 21> OperatorNames{
    "add", "sub", "mul", "div", "rem", "shl",  "shr",
    "and", "or",  "xor", "neg", "not", "lnot", "eq",
    "ne",  "lt",  "le",  "gt",  "ge",  "min",  "max"};

std::string_view toString(IntegerOp op) noexcept {
  const auto index = static_cast<std::size_t>(op);
  return index < OperatorNames.size() ? std::span(OperatorNames)[index]
                                      : "invalid";
}

std::optional<IntegerOp> parseIntegerOp(std::string_view text) {
  const auto *const it = std::ranges::find(OperatorNames, text);
  if (it == OperatorNames.end())
    return std::nullopt;
  return static_cast<IntegerOp>(std::distance(OperatorNames.begin(), it));
}

bool isUnary(IntegerOp op) noexcept {
  return op == IntegerOp::Negate || op == IntegerOp::Complement ||
         op == IntegerOp::LogicalNot;
}

bool isComparison(IntegerOp op) noexcept {
  return op >= IntegerOp::Equal && op <= IntegerOp::GreaterEqual;
}

IntegerOp negateComparison(IntegerOp op) noexcept {
  switch (op) {
  case IntegerOp::Equal:
    return IntegerOp::NotEqual;
  case IntegerOp::NotEqual:
    return IntegerOp::Equal;
  case IntegerOp::Less:
    return IntegerOp::GreaterEqual;
  case IntegerOp::LessEqual:
    return IntegerOp::Greater;
  case IntegerOp::Greater:
    return IntegerOp::LessEqual;
  case IntegerOp::GreaterEqual:
    return IntegerOp::Less;
  default:
    return op;
  }
}

IntegerOp reverseComparison(IntegerOp op) noexcept {
  switch (op) {
  case IntegerOp::Less:
    return IntegerOp::Greater;
  case IntegerOp::LessEqual:
    return IntegerOp::GreaterEqual;
  case IntegerOp::Greater:
    return IntegerOp::Less;
  case IntegerOp::GreaterEqual:
    return IntegerOp::LessEqual;
  default:
    return op;
  }
}

std::string_view toString(IntegerError error) noexcept {
  switch (error) {
  case IntegerError::None:
    return "";
  case IntegerError::SignedOverflow:
    return "signed integer overflow";
  case IntegerError::DivisionByZero:
    return "division by zero";
  case IntegerError::SignedDivisionOverflow:
    return "signed division overflow";
  case IntegerError::ShiftCount:
    return "invalid shift count";
  case IntegerError::SignedLeftShift:
    return "invalid signed left shift";
  case IntegerError::IncompatibleTypes:
    return "unsupported integer type";
  }
  return "unsupported integer operation";
}

IntegerEvaluation evaluateInteger(IntegerOp op, IntegerValue lhs,
                                  IntegerValue rhs, bool wrapSigned) {
  const auto fail = [](IntegerError error) -> IntegerEvaluation {
    return {.value = std::nullopt, .error = error};
  };
  const IntegerType type = lhs.type;
  const bool shift = op == IntegerOp::ShiftLeft || op == IntegerOp::ShiftRight;
  if (!type.valid() || !rhs.type.valid() ||
      (!isUnary(op) && !shift && type != rhs.type))
    return fail(IntegerError::IncompatibleTypes);
  const auto value = [type](std::uint64_t bits) -> IntegerEvaluation {
    return {.value = IntegerValue::ofBits(type, bits)};
  };
  const auto boolean = [](bool condition) -> IntegerEvaluation {
    return {.value = IntegerValue::ofBits(
                BooleanType, static_cast<std::uint64_t>(condition))};
  };
  const std::uint64_t a = lhs.bits;
  const std::uint64_t b = rhs.bits;
  const bool signedCheck = type.isSigned && !wrapSigned;
  const std::uint64_t positiveMax = type.signBit() - 1;
  switch (op) {
  case IntegerOp::Add: {
    const auto result = IntegerValue::ofBits(type, a + b);
    if (signedCheck && lhs.negative() == rhs.negative() &&
        result.negative() != lhs.negative())
      return fail(IntegerError::SignedOverflow);
    return {.value = result};
  }
  case IntegerOp::Subtract: {
    const auto result = IntegerValue::ofBits(type, a - b);
    if (signedCheck && lhs.negative() != rhs.negative() &&
        result.negative() != lhs.negative())
      return fail(IntegerError::SignedOverflow);
    return {.value = result};
  }
  case IntegerOp::Multiply:
    if (signedCheck) {
      const std::uint64_t limit =
          lhs.negative() != rhs.negative() ? type.signBit() : positiveMax;
      const auto divisor = rhs.magnitude();
      if (divisor != 0 && lhs.magnitude() > limit / divisor)
        return fail(IntegerError::SignedOverflow);
    }
    return value(a * b);
  case IntegerOp::Divide:
  case IntegerOp::Remainder: {
    const auto divisor = rhs.magnitude();
    if (divisor == 0)
      return fail(IntegerError::DivisionByZero);
    if (type.isSigned && a == type.signBit() && b == type.mask())
      return fail(IntegerError::SignedDivisionOverflow);
    const std::uint64_t mag = op == IntegerOp::Divide
                                  ? lhs.magnitude() / divisor
                                  : lhs.magnitude() % divisor;
    const bool negative = op == IntegerOp::Divide
                              ? lhs.negative() != rhs.negative()
                              : lhs.negative();
    return value(negative ? ~mag + 1 : mag);
  }
  case IntegerOp::ShiftLeft:
  case IntegerOp::ShiftRight: {
    if (rhs.negative() || b >= type.width)
      return fail(IntegerError::ShiftCount);
    const auto count = static_cast<unsigned>(b);
    if (op == IntegerOp::ShiftLeft) {
      if (type.isSigned && (lhs.negative() || a > (positiveMax >> count)))
        return fail(IntegerError::SignedLeftShift);
      return value(a << count);
    }
    if (lhs.negative() && count != 0)
      return value((a >> count) | (type.mask() ^ (type.mask() >> count)));
    return value(a >> count);
  }
  case IntegerOp::BitAnd:
    return value(a & b);
  case IntegerOp::BitOr:
    return value(a | b);
  case IntegerOp::BitXor:
    return value(a ^ b);
  case IntegerOp::Negate:
    if (signedCheck && a == type.signBit())
      return fail(IntegerError::SignedOverflow);
    return value(~a + 1);
  case IntegerOp::Complement:
    return value(~a);
  case IntegerOp::LogicalNot:
    return boolean(a == 0);
  case IntegerOp::Equal:
    return boolean(a == b);
  case IntegerOp::NotEqual:
    return boolean(a != b);
  case IntegerOp::Less:
    return boolean(type.rank(a) < type.rank(b));
  case IntegerOp::LessEqual:
    return boolean(type.rank(a) <= type.rank(b));
  case IntegerOp::Greater:
    return boolean(type.rank(a) > type.rank(b));
  case IntegerOp::GreaterEqual:
    return boolean(type.rank(a) >= type.rank(b));
  case IntegerOp::Minimum:
    return value(type.rank(a) <= type.rank(b) ? a : b);
  case IntegerOp::Maximum:
    return value(type.rank(a) >= type.rank(b) ? a : b);
  }
  return fail(IntegerError::IncompatibleTypes);
}

IntegerRange IntegerRange::full(IntegerType type) {
  return fromRanks(type, {{.lower = 0, .upper = type.mask()}});
}

IntegerRange IntegerRange::singleton(IntegerValue value) {
  const auto rank = value.type.rank(value.bits);
  return fromRanks(value.type, {{.lower = rank, .upper = rank}});
}

IntegerRange IntegerRange::between(IntegerValue lower, IntegerValue upper) {
  if (lower.type != upper.type)
    return full(lower.type);
  return fromRanks(lower.type, {{.lower = lower.type.rank(lower.bits),
                                 .upper = upper.type.rank(upper.bits)}});
}

IntegerRange IntegerRange::fromRanks(IntegerType type,
                                     std::vector<IntegerInterval> ranges) {
  IntegerRange result(type);
  if (!type.valid())
    return result;
  std::ranges::sort(ranges);
  for (const auto &range : ranges) {
    if (range.lower > range.upper || range.upper > type.mask())
      continue;
    if (!result.intervals.empty()) {
      auto &last = result.intervals.back();
      if (range.lower <= last.upper ||
          (last.upper != UINT64_MAX && range.lower == last.upper + 1)) {
        last.upper = std::max(last.upper, range.upper);
        continue;
      }
    }
    result.intervals.push_back(range);
  }
  if (result.intervals.size() > MaxIntegerIntervals)
    result.intervals = {{.lower = 0, .upper = type.mask()}};
  return result;
}

bool IntegerRange::isFull() const noexcept {
  return intervals.size() == 1 && intervals.front().lower == 0 &&
         intervals.front().upper == type.mask();
}

bool IntegerRange::contains(IntegerValue value) const noexcept {
  if (value.type != type)
    return false;
  const auto rank = type.rank(value.bits);
  return std::ranges::any_of(intervals, [rank](const auto &interval) {
    return rank >= interval.lower && rank <= interval.upper;
  });
}

bool IntegerRange::contains(const IntegerRange &other) const noexcept {
  if (type != other.type)
    return false;
  return std::ranges::all_of(other.intervals, [this](const auto &range) {
    return std::ranges::any_of(intervals, [&range](const auto &mine) {
      return mine.lower <= range.lower && mine.upper >= range.upper;
    });
  });
}

bool IntegerRange::disjoint(const IntegerRange &other) const noexcept {
  if (type != other.type)
    return false;
  for (const auto &a : intervals)
    for (const auto &b : other.intervals)
      if (a.lower <= b.upper && b.lower <= a.upper)
        return false;
  return true;
}

std::optional<IntegerValue> IntegerRange::constant() const noexcept {
  if (intervals.size() != 1 ||
      intervals.front().lower != intervals.front().upper)
    return std::nullopt;
  return IntegerValue::ofBits(type, type.rank(intervals.front().lower));
}

std::optional<IntegerValue> IntegerRange::minimum() const noexcept {
  if (empty())
    return std::nullopt;
  return IntegerValue::ofBits(type, type.rank(intervals.front().lower));
}

std::optional<IntegerValue> IntegerRange::maximum() const noexcept {
  if (empty())
    return std::nullopt;
  return IntegerValue::ofBits(type, type.rank(intervals.back().upper));
}

IntegerRange IntegerRange::converted(IntegerType destination) const {
  if (destination == type)
    return *this;
  if (destination.isBoolean) {
    const auto zero = IntegerValue::ofBits(type, 0);
    if (empty())
      return IntegerRange(destination);
    if (!contains(zero))
      return singleton(IntegerValue::ofBits(destination, 1));
    return constant() ? singleton(IntegerValue::ofBits(destination, 0))
                      : full(destination);
  }
  std::vector<IntegerInterval> ranges;
  for (const auto &interval : intervals) {
    const auto span = interval.upper - interval.lower;
    if (span >= destination.mask())
      return full(destination);
    const auto first = IntegerValue::ofBits(type, type.rank(interval.lower))
                           .converted(destination);
    const auto begin = destination.rank(first.bits);
    if (span <= destination.mask() - begin) {
      ranges.push_back({.lower = begin, .upper = begin + span});
    } else {
      ranges.push_back({.lower = begin, .upper = destination.mask()});
      ranges.push_back(
          {.lower = 0, .upper = span - (destination.mask() - begin) - 1});
    }
  }
  return fromRanks(destination, std::move(ranges));
}

IntegerRange IntegerRange::intersect(const IntegerRange &other) const {
  if (type != other.type)
    return *this;
  std::vector<IntegerInterval> ranges;
  for (const auto &a : intervals)
    for (const auto &b : other.intervals) {
      const auto lower = std::max(a.lower, b.lower);
      const auto upper = std::min(a.upper, b.upper);
      if (lower <= upper)
        ranges.push_back({.lower = lower, .upper = upper});
    }
  return fromRanks(type, std::move(ranges));
}

IntegerRange IntegerRange::united(const IntegerRange &other) const {
  if (type != other.type)
    return full(type);
  auto ranges = intervals;
  ranges.insert(ranges.end(), other.intervals.begin(), other.intervals.end());
  return fromRanks(type, std::move(ranges));
}

IntegerRange IntegerRange::widened(const IntegerRange &other) const {
  if (type != other.type)
    return full(type);
  if (empty())
    return other;
  if (other.empty() || contains(other))
    return *this;
  const auto lower = other.intervals.front().lower < intervals.front().lower
                         ? 0
                         : intervals.front().lower;
  const auto upper = other.intervals.back().upper > intervals.back().upper
                         ? type.mask()
                         : intervals.back().upper;
  return fromRanks(type, {{.lower = lower, .upper = upper}});
}

IntegerRange IntegerRange::satisfying(IntegerOp op,
                                      const IntegerRange &rhs) const {
  if (type != rhs.type || !isComparison(op))
    return *this;
  if (empty() || rhs.empty())
    return IntegerRange(type);
  if (op == IntegerOp::Equal)
    return intersect(rhs);
  if (op == IntegerOp::NotEqual) {
    const auto value = rhs.constant();
    if (!value)
      return *this;
    const auto at = type.rank(value->bits);
    std::vector<IntegerInterval> ranges;
    for (const auto &range : intervals) {
      if (at < range.lower || at > range.upper) {
        ranges.push_back(range);
      } else {
        if (range.lower < at)
          ranges.push_back({.lower = range.lower, .upper = at - 1});
        if (at < range.upper)
          ranges.push_back({.lower = at + 1, .upper = range.upper});
      }
    }
    return fromRanks(type, std::move(ranges));
  }
  std::uint64_t lower = 0;
  std::uint64_t upper = type.mask();
  if (op == IntegerOp::Less || op == IntegerOp::LessEqual) {
    upper = rhs.intervals.back().upper;
    if (op == IntegerOp::Less) {
      if (upper == 0)
        return IntegerRange(type);
      --upper;
    }
  } else {
    lower = rhs.intervals.front().lower;
    if (op == IntegerOp::Greater) {
      if (lower == type.mask())
        return IntegerRange(type);
      ++lower;
    }
  }
  return intersect(fromRanks(type, {{.lower = lower, .upper = upper}}));
}

std::string IntegerRange::toString() const {
  std::string text = type.toString() + ":";
  for (const auto &range : intervals) {
    if (text.back() != ':')
      text += ',';
    text += std::to_string(range.lower) + "-" + std::to_string(range.upper);
  }
  return text;
}

std::optional<IntegerRange> IntegerRange::parse(std::string_view text) {
  if (text.size() > 128)
    return std::nullopt;
  const auto colon = text.find(':');
  if (colon == std::string_view::npos)
    return std::nullopt;
  const auto type = IntegerType::parse(text.substr(0, colon));
  if (!type)
    return std::nullopt;
  text.remove_prefix(colon + 1);
  std::vector<IntegerInterval> ranges;
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto part = text.substr(0, comma);
    const auto dash = part.find('-');
    if (dash == std::string_view::npos || ranges.size() >= MaxIntegerIntervals)
      return std::nullopt;
    const auto lower = unsignedNumber(part.substr(0, dash));
    const auto upper = unsignedNumber(part.substr(dash + 1));
    if (!lower || !upper || *lower > *upper || *upper > type->mask())
      return std::nullopt;
    if (!ranges.empty() && (ranges.back().upper == UINT64_MAX ||
                            *lower <= ranges.back().upper + 1))
      return std::nullopt;
    ranges.push_back({.lower = *lower, .upper = *upper});
    if (comma == std::string_view::npos)
      break;
    text.remove_prefix(comma + 1);
    if (text.empty())
      return std::nullopt;
  }
  return fromRanks(*type, std::move(ranges));
}

static std::optional<std::vector<IntegerValue>>
smallValues(const IntegerRange &range) {
  std::vector<IntegerValue> result;
  for (const auto &interval : range.all()) {
    if (interval.upper - interval.lower >= 16 ||
        result.size() +
                static_cast<std::size_t>(interval.upper - interval.lower) + 1 >
            16)
      return std::nullopt;
    for (auto rank = interval.lower;; ++rank) {
      result.push_back(IntegerValue::ofBits(range.type, range.type.rank(rank)));
      if (rank == interval.upper)
        break;
    }
  }
  return result;
}

static IntegerRangeEvaluation
enumerateInteger(IntegerOp op, const std::vector<IntegerValue> &lhs,
                 const std::vector<IntegerValue> &rhs, IntegerType resultType,
                 bool wrapSigned) {
  std::vector<IntegerInterval> ranges;
  bool invalid = false;
  IntegerError error = IntegerError::None;
  for (const auto a : lhs) {
    for (const auto b : rhs) {
      const auto result = evaluateInteger(op, a, b, wrapSigned);
      if (result.value) {
        const auto rank = resultType.rank(result.value->bits);
        ranges.push_back({.lower = rank, .upper = rank});
      } else {
        invalid = true;
        if (error == IntegerError::None)
          error = result.error;
      }
      if (isUnary(op))
        break;
    }
  }
  // A possibly undefined execution must not become a path-pruning premise.
  const bool always = invalid && ranges.empty();
  return {.values =
              invalid ? IntegerRange::full(resultType)
                      : IntegerRange::fromRanks(resultType, std::move(ranges)),
          .mayBeInvalid = invalid,
          .alwaysInvalid = always,
          .error = error};
}

IntegerRangeEvaluation evaluateInteger(IntegerOp op, const IntegerRange &lhs,
                                       const IntegerRange &rhs,
                                       bool wrapSigned) {
  const auto type = lhs.type;
  const auto resultType =
      isComparison(op) || op == IntegerOp::LogicalNot ? BooleanType : type;
  const auto unknown = [resultType](bool invalid = false,
                                    IntegerError error = IntegerError::None,
                                    bool always =
                                        false) -> IntegerRangeEvaluation {
    return {.values = IntegerRange::full(resultType),
            .mayBeInvalid = invalid,
            .alwaysInvalid = always,
            .error = error};
  };
  if (lhs.empty() || rhs.empty())
    return {.values = IntegerRange(resultType)};
  const bool shift = op == IntegerOp::ShiftLeft || op == IntegerOp::ShiftRight;
  if (!isUnary(op) && !shift && type != rhs.type)
    return unknown(true, IntegerError::IncompatibleTypes);
  const auto aValues = smallValues(lhs);
  const auto bValues =
      isUnary(op) ? std::optional(std::vector{IntegerValue::ofBits(type, 0)})
                  : smallValues(rhs);
  if (aValues && bValues)
    return enumerateInteger(op, *aValues, *bValues, resultType, wrapSigned);
  const auto aMin = *lhs.minimum();
  const auto aMax = *lhs.maximum();
  const auto bMin = *rhs.minimum();
  const auto bMax = *rhs.maximum();
  if (isComparison(op)) {
    if (lhs.satisfying(op, rhs).empty())
      return {.values = IntegerRange::singleton(
                  IntegerValue::ofBits(BooleanType, 0))};
    if (lhs.satisfying(negateComparison(op), rhs).empty())
      return {.values = IntegerRange::singleton(
                  IntegerValue::ofBits(BooleanType, 1))};
    return unknown();
  }
  if (op == IntegerOp::LogicalNot) {
    if (!lhs.contains(IntegerValue::ofBits(type, 0)))
      return {.values = IntegerRange::singleton(
                  IntegerValue::ofBits(BooleanType, 0))};
    return unknown();
  }
  if (op == IntegerOp::Minimum || op == IntegerOp::Maximum) {
    const auto low = evaluateInteger(op, aMin, bMin);
    const auto high = evaluateInteger(op, aMax, bMax);
    return {.values = IntegerRange::between(*low.value, *high.value)};
  }
  if (op == IntegerOp::Add || op == IntegerOp::Subtract) {
    std::vector<IntegerInterval> ranges;
    for (const auto &a : lhs.all()) {
      for (const auto &b : rhs.all()) {
        const auto first = evaluateInteger(
            op, IntegerValue::ofBits(type, type.rank(a.lower)),
            IntegerValue::ofBits(
                type, type.rank(op == IntegerOp::Add ? b.lower : b.upper)),
            wrapSigned);
        const auto last = evaluateInteger(
            op, IntegerValue::ofBits(type, type.rank(a.upper)),
            IntegerValue::ofBits(
                type, type.rank(op == IntegerOp::Add ? b.upper : b.lower)),
            wrapSigned);
        if (!first.value || !last.value)
          return unknown(true, IntegerError::SignedOverflow);
        const auto spanA = a.upper - a.lower;
        const auto spanB = b.upper - b.lower;
        if (spanA >= type.mask() - spanB)
          return unknown();
        const auto span = spanA + spanB;
        const auto begin = type.rank(first.value->bits);
        if (span <= type.mask() - begin) {
          ranges.push_back({.lower = begin, .upper = begin + span});
        } else {
          ranges.push_back({.lower = begin, .upper = type.mask()});
          ranges.push_back(
              {.lower = 0, .upper = span - (type.mask() - begin) - 1});
        }
      }
    }
    return {.values = IntegerRange::fromRanks(type, std::move(ranges))};
  }
  if (op == IntegerOp::Multiply) {
    // Scale each modular interval before taking a hull. A signed-to-unsigned
    // conversion can produce two separated intervals; collapsing them first
    // would invent zero in a nonzero converted count times a small constant.
    const auto lhsConstant = lhs.constant();
    const auto rhsConstant = rhs.constant();
    if (!type.isSigned && (lhsConstant || rhsConstant)) {
      const auto factor = lhsConstant ? lhsConstant->bits : rhsConstant->bits;
      const auto &input = lhsConstant ? rhs : lhs;
      if (factor == 0)
        return {.values =
                    IntegerRange::singleton(IntegerValue::ofBits(type, 0))};
      std::vector<IntegerInterval> ranges;
      for (const auto &interval : input.all()) {
        const auto span = interval.upper - interval.lower;
        if (span > type.mask() / factor)
          return unknown();
        const auto distance = span * factor;
        const auto begin = (interval.lower * factor) & type.mask();
        if (distance <= type.mask() - begin) {
          ranges.push_back({.lower = begin, .upper = begin + distance});
        } else {
          ranges.push_back({.lower = begin, .upper = type.mask()});
          ranges.push_back(
              {.lower = 0, .upper = distance - (type.mask() - begin) - 1});
        }
      }
      return {.values = IntegerRange::fromRanks(type, std::move(ranges))};
    }
    if (!type.isSigned && bMax.bits != 0 && aMax.bits > type.mask() / bMax.bits)
      return unknown();
    std::uint64_t lower = type.mask();
    std::uint64_t upper = 0;
    for (const auto a : {aMin, aMax})
      for (const auto b : {bMin, bMax}) {
        const auto result = evaluateInteger(op, a, b, wrapSigned);
        if (!result.value)
          return unknown(true, result.error);
        if (type.isSigned && wrapSigned) {
          const auto checked = evaluateInteger(op, a, b, false);
          if (!checked.value)
            return unknown();
        }
        lower = std::min(lower, type.rank(result.value->bits));
        upper = std::max(upper, type.rank(result.value->bits));
      }
    return {.values = IntegerRange::fromRanks(
                type, {{.lower = lower, .upper = upper}})};
  }
  if (op == IntegerOp::Divide || op == IntegerOp::Remainder) {
    if (bMin.bits == 0 || bMax.bits == 0 ||
        rhs.contains(IntegerValue::ofBits(rhs.type, 0)))
      return unknown(true, IntegerError::DivisionByZero,
                     rhs.constant().has_value());
    if (type.isSigned &&
        lhs.contains(IntegerValue::ofBits(type, type.signBit())) &&
        rhs.contains(IntegerValue::ofBits(type, type.mask())))
      return unknown(true, IntegerError::SignedDivisionOverflow);
    if (!aMin.negative() && !bMin.negative()) {
      if (op == IntegerOp::Divide)
        return {.values = IntegerRange::between(
                    IntegerValue::ofBits(type, aMin.bits / bMax.bits),
                    IntegerValue::ofBits(type, aMax.bits / bMin.bits))};
      return {
          .values = IntegerRange::between(
              IntegerValue::ofBits(type, 0),
              IntegerValue::ofBits(type, std::min(aMax.bits, bMax.bits - 1)))};
    }
    return unknown();
  }
  if (shift) {
    if (bMin.negative() || bMax.bits >= type.width)
      return unknown(true, IntegerError::ShiftCount,
                     !bMin.negative() && bMin.bits >= type.width);
    if (op == IntegerOp::ShiftLeft && type.isSigned &&
        (aMin.negative() || aMax.bits > ((type.signBit() - 1) >> bMax.bits)))
      return unknown(true, IntegerError::SignedLeftShift);
    if (op == IntegerOp::ShiftRight && rhs.constant())
      return {.values = IntegerRange::between(
                  *evaluateInteger(op, aMin, bMin, wrapSigned).value,
                  *evaluateInteger(op, aMax, bMin, wrapSigned).value)};
    if (op == IntegerOp::ShiftLeft && !aMin.negative() &&
        aMax.bits <= (type.mask() >> bMax.bits))
      return {.values = IntegerRange::between(
                  IntegerValue::ofBits(type, aMin.bits << bMin.bits),
                  IntegerValue::ofBits(type, aMax.bits << bMax.bits))};
    return unknown();
  }
  if (op == IntegerOp::Negate) {
    if (type.isSigned && !wrapSigned &&
        lhs.contains(IntegerValue::ofBits(type, type.signBit())))
      return unknown(true, IntegerError::SignedOverflow);
    const auto zero = IntegerRange::singleton(IntegerValue::ofBits(type, 0));
    return evaluateInteger(IntegerOp::Subtract, zero, lhs, wrapSigned);
  }
  if (op == IntegerOp::Complement) {
    std::vector<IntegerInterval> ranges;
    for (const auto &interval : lhs.all())
      ranges.push_back({.lower = type.mask() - interval.upper,
                        .upper = type.mask() - interval.lower});
    return {.values = IntegerRange::fromRanks(type, std::move(ranges))};
  }
  if (op == IntegerOp::BitAnd && (!aMin.negative() || !bMin.negative())) {
    // RFC 0017/0019: a nonnegative mask clears the sign bit even if the
    // other operand is signed or crosses zero. Only nonnegative operands
    // supply numeric upper bounds on the resulting bit pattern.
    auto upper = type.mask();
    if (!aMin.negative())
      upper = std::min(upper, aMax.bits);
    if (!bMin.negative())
      upper = std::min(upper, bMax.bits);
    return {.values = IntegerRange::between(IntegerValue::ofBits(type, 0),
                                            IntegerValue::ofBits(type, upper))};
  }
  return unknown();
}

} // namespace weavec::core
