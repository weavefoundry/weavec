//===- Footprint.h - Allocation conservation (RFC 0027) --------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_FOOTPRINT_H
#define WEAVEC_CORE_FOOTPRINT_H

#include "weavec/Core/Place.h"

#include <cstdint>
#include <map>
#include <vector>

namespace weavec::core {

inline constexpr std::size_t MaxFootprintVariables = 64;
inline constexpr std::size_t MaxFootprintRelations = 64;

/// Formal sums of allocation identities, NOT numerical allocation counts.
/// A structural proof must justify each disjoint split and release separately.
using FootprintSum = std::map<PlaceId, std::int64_t>;

/// Homogeneous must equalities, sum(coeff * footprint(place)) == empty.
/// Rows are a canonical sparse basis over exact rational coefficients. Integral
/// fraction-free elimination detects overflow and loses proof on exhaustion.
/// Join is intersection of the represented row spaces, not intersection of
/// their written rows: differently partitioned paths can conserve one input.
class FootprintRelations {
public:
  bool constrain(FootprintSum equation);
  [[nodiscard]] bool entails(FootprintSum equation) const;
  [[nodiscard]] bool equal(PlaceId first, PlaceId second) const;
  [[nodiscard]] bool empty(PlaceId place) const;
  void forget(PlaceId place);
  /// Simultaneous value assignment, including self updates such as x = x + y.
  void assign(PlaceId destination, FootprintSum value);
  bool join(const FootprintRelations &other);
  void clear();
  [[nodiscard]] bool limited() const { return exhausted; }
  [[nodiscard]] const std::vector<FootprintSum> &all() const { return rows; }
  friend bool operator==(const FootprintRelations &,
                         const FootprintRelations &) = default;

private:
  std::vector<FootprintSum> rows;
  bool exhausted = false;
  bool canonicalize();
  void fail();
};

} // namespace weavec::core
#endif // WEAVEC_CORE_FOOTPRINT_H
