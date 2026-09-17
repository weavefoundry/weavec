//===- Interface.h - Portable C storage descriptions (RFC 0028) -*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef WEAVEC_CORE_INTERFACE_H
#define WEAVEC_CORE_INTERFACE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {

enum class InterfaceKind : std::uint8_t {
  Void,
  Integer,
  Floating,
  Pointer,
  Function,
  Record,
  Array
};

struct InterfaceField {
  std::string name;
  std::uint32_t type = 0;
  std::uint64_t offset = 0;
  friend bool operator==(const InterfaceField &,
                         const InterfaceField &) = default;
};

/// Immutable representation information, never a memory permission.
struct InterfaceNode {
  InterfaceKind kind = InterfaceKind::Void;
  std::uint64_t bytes = 0;
  std::uint64_t alignment = 0;
  std::uint64_t count = 0;
  std::uint32_t element = 0;
  unsigned qualifiers = 0;
  bool variadic = false;
  bool prototype = true;
  std::string name;
  std::string view;
  std::string typedefName;
  std::vector<std::uint32_t> parameters;
  std::vector<InterfaceField> fields;
  friend bool operator==(const InterfaceNode &,
                         const InterfaceNode &) = default;
};

inline constexpr std::size_t MaxInterfaceNodes = 128;
inline constexpr std::size_t MaxInterfaceFields = 64;
inline constexpr std::size_t MaxInterfaceBytes = 65536;

/// Node zero is the root. Pointer cycles are legal; by-value cycles are not.
struct InterfaceType {
  std::vector<InterfaceNode> nodes;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] std::string encode() const;
  [[nodiscard]] static std::optional<InterfaceType> decode(std::string_view);
  friend bool operator==(const InterfaceType &,
                         const InterfaceType &) = default;
};

/// A conflicting publication stays conflicted, independent of import order.
using InterfaceTypes = std::map<std::string, std::optional<InterfaceType>>;
void mergeInterfaceTypes(InterfaceTypes &into, const InterfaceTypes &from);

} // namespace weavec::core
#endif
