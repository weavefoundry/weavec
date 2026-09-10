//===- Traversal.h - Checked traversal relations (RFC 0021) ------*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_TRAVERSAL_H
#define WEAVEC_CORE_TRAVERSAL_H

#include "weavec/Core/Integer.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Relation.h"

#include <map>
#include <optional>
#include <utility>

namespace weavec::core {

inline constexpr std::size_t MaxTraversalVariables = 64;
inline constexpr std::size_t MaxTraversalSteps = 4096;
inline constexpr unsigned MaxTraversalIterations = 32;

/// An absent term denotes mathematical zero, independently of PlaceId 0.
using DifferenceTerm = std::optional<PlaceId>;

/// Must-facts x - y <= bound. Arithmetic here is mathematical, not C wrap.
/// Callers establish value preservation before installing or updating a fact.
class DifferenceConstraints {
public:
  using Key = std::pair<DifferenceTerm, DifferenceTerm>;
  bool constrain(DifferenceTerm x, DifferenceTerm y, std::int64_t bound);
  void learn(PlaceId lhs, RelationEdge edge, PlaceId rhs);
  [[nodiscard]] std::optional<std::int64_t> bound(DifferenceTerm x,
                                                  DifferenceTerm y) const;
  [[nodiscard]] bool implies(DifferenceTerm x, DifferenceTerm y,
                             std::int64_t limit) const;
  void forget(PlaceId place);
  void assign(PlaceId dest, DifferenceTerm source, std::int64_t offset);
  bool join(const DifferenceConstraints &other);
  [[nodiscard]] bool limited() const noexcept { return exhausted; }
  [[nodiscard]] bool empty() const noexcept { return constraints.empty(); }
  friend bool operator==(const DifferenceConstraints &,
                         const DifferenceConstraints &) = default;

private:
  std::map<Key, std::int64_t> constraints;
  mutable bool exhausted = false;
};

/// Same-array provenance/lifetime are separate frontend obligations. This
/// function checks byte divisibility and ptrdiff_t representability only.
[[nodiscard]] std::optional<IntegerValue>
pointerDifference(std::int64_t leftBytes, std::int64_t rightBytes,
                  std::int64_t elementBytes, IntegerType differenceType);

} // namespace weavec::core

#endif // WEAVEC_CORE_TRAVERSAL_H
