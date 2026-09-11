//===- ObjectType.h - Checked object views (RFC 0022) -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_OBJECTTYPE_H
#define WEAVEC_CORE_OBJECTTYPE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace weavec::core {

/// Target layout and canonical compatible type, supplied by Analysis.
/// This describes a view only; no lifetime or initialized bytes are implied.
struct ObjectType {
  std::uint64_t bytes = 0;
  std::uint64_t alignment = 0;
  std::string identity;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] static std::optional<ObjectType> parse(std::string_view text);
  [[nodiscard]] bool accepts(const ObjectType &view, std::int64_t offset) const;
  friend bool operator==(const ObjectType &, const ObjectType &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_OBJECTTYPE_H
