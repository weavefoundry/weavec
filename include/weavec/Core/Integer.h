//===- Integer.h - Target integer values and bounded ranges ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0017. Bit patterns belong to a source target type, never to the host's
// interpretation of an int64_t. Ranges are unions of at most two intervals
// in numeric order (signed order for signed types).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_INTEGER_H
#define WEAVEC_CORE_INTEGER_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

struct IntegerType {
  unsigned width = 32;
  bool isSigned = true;
  bool isBoolean = false;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t mask() const noexcept;
  [[nodiscard]] std::uint64_t signBit() const noexcept;
  /// Bit pattern <-> numeric-order position (both are their own inverse).
  [[nodiscard]] std::uint64_t rank(std::uint64_t bits) const noexcept;
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<IntegerType> parse(std::string_view text);

  friend auto operator<=>(const IntegerType &, const IntegerType &) = default;
};

inline constexpr IntegerType BooleanType{
    .width = 1, .isSigned = false, .isBoolean = true};

struct IntegerValue {
  IntegerType type;
  std::uint64_t bits = 0;

  [[nodiscard]] static IntegerValue ofBits(IntegerType type,
                                           std::uint64_t bits) noexcept;
  [[nodiscard]] bool negative() const noexcept;
  [[nodiscard]] std::uint64_t magnitude() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> signedValue() const noexcept;
  [[nodiscard]] IntegerValue converted(IntegerType destination) const noexcept;
  [[nodiscard]] std::string toString() const;

  friend auto operator<=>(const IntegerValue &, const IntegerValue &) = default;
};

enum class IntegerOp : std::uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  ShiftLeft,
  ShiftRight,
  BitAnd,
  BitOr,
  BitXor,
  Negate,
  Complement,
  LogicalNot,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Minimum,
  Maximum,
};

[[nodiscard]] std::string_view toString(IntegerOp op) noexcept;
[[nodiscard]] std::optional<IntegerOp> parseIntegerOp(std::string_view text);
[[nodiscard]] bool isUnary(IntegerOp op) noexcept;
[[nodiscard]] bool isComparison(IntegerOp op) noexcept;
[[nodiscard]] IntegerOp negateComparison(IntegerOp op) noexcept;
[[nodiscard]] IntegerOp reverseComparison(IntegerOp op) noexcept;

enum class IntegerError : std::uint8_t {
  None,
  SignedOverflow,
  DivisionByZero,
  SignedDivisionOverflow,
  ShiftCount,
  SignedLeftShift,
  IncompatibleTypes,
};

[[nodiscard]] std::string_view toString(IntegerError error) noexcept;

struct IntegerEvaluation {
  std::optional<IntegerValue> value;
  IntegerError error = IntegerError::None;
};

/// Operands have already undergone the source language's promotions.
/// Shift counts may have a different type. Comparisons yield BooleanType.
[[nodiscard]] IntegerEvaluation evaluateInteger(IntegerOp op, IntegerValue lhs,
                                                IntegerValue rhs,
                                                bool wrapSigned = false);

struct IntegerInterval {
  std::uint64_t lower = 0;
  std::uint64_t upper = 0;
  friend auto operator<=>(const IntegerInterval &,
                          const IntegerInterval &) = default;
};

inline constexpr std::size_t MaxIntegerIntervals = 2;

class IntegerRange {
public:
  IntegerType type;

  /// Empty is bottom (no represented value), not unknown.
  IntegerRange() = default;
  explicit IntegerRange(IntegerType type) : type(type) {}
  [[nodiscard]] static IntegerRange full(IntegerType type);
  [[nodiscard]] static IntegerRange singleton(IntegerValue value);
  [[nodiscard]] static IntegerRange between(IntegerValue lower,
                                            IntegerValue upper);
  [[nodiscard]] static IntegerRange
  fromRanks(IntegerType type, std::vector<IntegerInterval> ranges);

  [[nodiscard]] bool empty() const noexcept { return intervals.empty(); }
  [[nodiscard]] bool isFull() const noexcept;
  [[nodiscard]] bool contains(IntegerValue value) const noexcept;
  [[nodiscard]] bool contains(const IntegerRange &other) const noexcept;
  [[nodiscard]] bool disjoint(const IntegerRange &other) const noexcept;
  [[nodiscard]] std::optional<IntegerValue> constant() const noexcept;
  [[nodiscard]] std::optional<IntegerValue> minimum() const noexcept;
  [[nodiscard]] std::optional<IntegerValue> maximum() const noexcept;
  [[nodiscard]] const std::vector<IntegerInterval> &all() const noexcept {
    return intervals;
  }
  [[nodiscard]] IntegerRange converted(IntegerType destination) const;
  [[nodiscard]] IntegerRange intersect(const IntegerRange &other) const;
  [[nodiscard]] IntegerRange united(const IntegerRange &other) const;
  /// Extrapolate changing bounds to the type endpoints at a loop join.
  [[nodiscard]] IntegerRange widened(const IntegerRange &other) const;
  /// Values of this operand for which `this op rhs` can hold.
  [[nodiscard]] IntegerRange satisfying(IntegerOp op,
                                        const IntegerRange &rhs) const;
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<IntegerRange> parse(std::string_view text);

  friend auto operator<=>(const IntegerRange &, const IntegerRange &) = default;

private:
  std::vector<IntegerInterval> intervals;
};

struct IntegerRangeEvaluation {
  IntegerRange values;
  bool mayBeInvalid = false;
  bool alwaysInvalid = false;
  IntegerError error = IntegerError::None;
};

/// GCC/Clang checked arithmetic first computes an infinite-precision integer,
/// then stores its conversion and reports whether the destination can hold it.
struct CheckedIntegerValue {
  IntegerValue value;
  bool overflow = false;
};
struct CheckedIntegerRange {
  IntegerRange values;
  IntegerRange overflow;
};
[[nodiscard]] std::optional<CheckedIntegerValue>
evaluateCheckedInteger(IntegerOp op, IntegerValue lhs, IntegerValue rhs,
                       IntegerType destination);
[[nodiscard]] CheckedIntegerRange
evaluateCheckedInteger(IntegerOp op, const IntegerRange &lhs,
                       const IntegerRange &rhs, IntegerType destination);

/// Sound range transfer. Unsupported precision widens, never invents a fact.
[[nodiscard]] IntegerRangeEvaluation evaluateInteger(IntegerOp op,
                                                     const IntegerRange &lhs,
                                                     const IntegerRange &rhs,
                                                     bool wrapSigned = false);

} // namespace weavec::core

#endif // WEAVEC_CORE_INTEGER_H
