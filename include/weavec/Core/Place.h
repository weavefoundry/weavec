//===- Place.h - Abstract memory places ------------------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A "place" is an abstract, frontend-neutral handle for a storage location
// (a local variable, parameter, field path, ...). The Clang integration layer
// maps `clang::ValueDecl`s and expressions onto places; the core model only
// ever reasons about `PlaceId`s.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_PLACE_H
#define WEAVEC_CORE_PLACE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

/// Opaque identifier for a place within one analysis unit.
struct PlaceId {
  std::uint32_t value = 0;

  friend constexpr bool operator==(PlaceId, PlaceId) noexcept = default;
};

struct PlaceIdHash {
  std::size_t operator()(PlaceId id) const noexcept {
    return std::hash<std::uint32_t>{}(id.value);
  }
};

/// Interns places and records their user-facing names for diagnostics.
class PlaceTable {
public:
  /// Creates a new place with the given display name (e.g. a variable name).
  [[nodiscard]] PlaceId create(std::string displayName);

  /// Display name for `id`.
  [[nodiscard]] std::string_view name(PlaceId id) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return names.size(); }

private:
  std::vector<std::string> names;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_PLACE_H
