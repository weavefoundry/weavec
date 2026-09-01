//===- Lifetime.h - Lifetime regions and outlives constraints ---*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lifetimes are opaque regions ordered by an "outlives" relation. Inference
// generates constraints of the form `'a: 'b` (a outlives b); the checker
// queries the transitive closure to validate borrows.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_LIFETIME_H
#define WEAVEC_CORE_LIFETIME_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace weavec::core {

/// Identifies a lifetime region within a single analysis unit.
struct LifetimeId {
  std::uint32_t value = 0;

  /// The `'static` lifetime, which outlives every other lifetime.
  static constexpr LifetimeId staticLifetime() noexcept { return {0}; }

  [[nodiscard]] constexpr bool isStatic() const noexcept { return value == 0; }

  friend constexpr bool operator==(LifetimeId, LifetimeId) noexcept = default;
};

/// A set of `outlives` constraints between lifetimes with transitive queries.
///
/// The `'static` lifetime (id 0) implicitly outlives all others and every
/// lifetime outlives itself.
class LifetimeConstraints {
public:
  LifetimeConstraints();

  /// Allocates a fresh, unconstrained lifetime.
  [[nodiscard]] LifetimeId fresh(std::string debugName = {});

  /// Records the constraint `longer: shorter` (longer outlives shorter).
  void addOutlives(LifetimeId longer, LifetimeId shorter);

  /// Returns true if `longer` provably outlives `shorter`.
  [[nodiscard]] bool outlives(LifetimeId longer, LifetimeId shorter) const;

  /// Returns the debug name given to `id`, or a synthesized one.
  [[nodiscard]] std::string name(LifetimeId id) const;

  /// Number of lifetimes allocated, including `'static`.
  [[nodiscard]] std::size_t size() const noexcept { return names.size(); }

private:
  std::vector<std::string> names;
  // Adjacency: longer -> set of lifetimes it directly outlives.
  std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> edges;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_LIFETIME_H
