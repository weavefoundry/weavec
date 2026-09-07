//===- Array.h - Bounded array selections and intervals --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_ARRAY_H
#define WEAVEC_CORE_ARRAY_H

#include "weavec/Core/Scalar.h"
#include "weavec/Core/Spatial.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace weavec::core {

inline constexpr std::size_t MaxArrayCells = 32;
inline constexpr std::size_t MaxArrayRanges = 32;

/// RFC 0015: a constant, or an immutable scalar identity plus a constant.
/// Symbols are local place numbers in the state and entry parameter numbers
/// in a summary. Analysis translates between those namespaces at calls.
struct ArrayIndex {
  // Keep an explicit aggregate default for callers using designated fields.
  // NOLINTNEXTLINE(readability-redundant-member-init)
  std::optional<std::uint32_t> symbol = {};
  std::int64_t offset = 0;

  [[nodiscard]] static ArrayIndex constant(std::int64_t value) {
    return {.symbol = std::nullopt, .offset = value};
  }
  [[nodiscard]] static ArrayIndex variable(std::uint32_t value,
                                           std::int64_t offset = 0) {
    return {.symbol = value, .offset = offset};
  }
  [[nodiscard]] std::optional<ArrayIndex> shifted(std::int64_t by) const;
  /// Difference when both selectors have the same symbolic part.
  [[nodiscard]] std::optional<std::int64_t>
  difference(const ArrayIndex &other) const;
  /// A constant (`3`) or a symbol (`$2+3`, `$2-1`, `$2`).
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<ArrayIndex> parse(std::string_view text);
  friend auto operator<=>(const ArrayIndex &, const ArrayIndex &) = default;
};

enum class ArrayRelation : std::uint8_t { Yes, No, Unknown };

/// Half-open range [begin, end). Work is bounded by facts, never its length.
struct ArrayInterval {
  ArrayIndex begin;
  ArrayIndex end;
  [[nodiscard]] std::optional<std::int64_t> length() const;
  [[nodiscard]] ArrayRelation contains(const ArrayIndex &index) const;
  [[nodiscard]] ArrayRelation overlaps(const ArrayInterval &other) const;
  [[nodiscard]] std::optional<ArrayInterval> shifted(std::int64_t by) const;
  friend auto operator<=>(const ArrayInterval &,
                          const ArrayInterval &) = default;
};

/// A symbolic contiguous range. Unlike an interval, its count may name a
/// different immutable scalar from its starting selector.
struct ArraySpan {
  ArrayIndex begin;
  Affine count;
  [[nodiscard]] ArrayRelation contains(const ArrayIndex &index,
                                       const ScalarTracker &scalars,
                                       const RelationTracker &relations) const;
  friend bool operator==(const ArraySpan &, const ArraySpan &) = default;
};

/// Translate an index from one range to the corresponding source cell.
[[nodiscard]] std::optional<ArrayIndex>
translateArrayIndex(const ArrayIndex &index, const ArrayIndex &from,
                    const ArrayIndex &to);
[[nodiscard]] bool arrayIndicesDisjoint(const ArrayIndex &a,
                                        const ArrayIndex &b,
                                        const ScalarTracker &scalars,
                                        const RelationTracker &relations);

} // namespace weavec::core

#endif // WEAVEC_CORE_ARRAY_H
