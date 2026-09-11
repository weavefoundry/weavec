//===- ObjectType.cpp - Portable object-view evidence (RFC 0022) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/ObjectType.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>

namespace weavec::core {

bool ObjectType::valid() const {
  return bytes > 0 && bytes <= std::numeric_limits<std::int64_t>::max() &&
         std::has_single_bit(alignment) && alignment <= bytes &&
         bytes % alignment == 0 && !identity.empty() &&
         identity.size() <= 8192 &&
         std::ranges::all_of(
             identity, [](unsigned char c) { return c >= 32 && c < 127; });
}

std::string ObjectType::toString() const {
  return valid() ? std::to_string(bytes) + ":" + std::to_string(alignment) +
                       ":" + identity
                 : std::string{};
}

std::optional<ObjectType> ObjectType::parse(std::string_view text) {
  if (text.size() > 8240)
    return std::nullopt;
  ObjectType result;
  const auto number = [&](std::uint64_t &out) {
    const auto end = text.find(':');
    if (end == std::string_view::npos || end == 0 || end > 20)
      return false;
    const auto part = text.substr(0, end);
    const auto parsed =
        std::from_chars(part.data(), part.data() + part.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
        part != std::to_string(out))
      return false;
    text.remove_prefix(end + 1);
    return true;
  };
  if (!number(result.bytes) || !number(result.alignment))
    return std::nullopt;
  result.identity = text;
  return result.valid() ? std::optional(result) : std::nullopt;
}

bool ObjectType::accepts(const ObjectType &view, std::int64_t offset) const {
  return valid() && view.valid() && *this == view && offset >= 0 &&
         static_cast<std::uint64_t>(offset) % bytes == 0;
}

} // namespace weavec::core
