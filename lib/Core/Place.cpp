//===- Place.cpp - Abstract memory places ---------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Place.h"

#include <utility>

namespace weavec::core {

PlaceId PlaceTable::create(std::string displayName) {
  const auto id = static_cast<std::uint32_t>(names.size());
  names.push_back(std::move(displayName));
  return PlaceId{id};
}

std::string_view PlaceTable::name(PlaceId id) const noexcept {
  if (id.value < names.size())
    return names[id.value];
  return "<unknown place>";
}

} // namespace weavec::core
