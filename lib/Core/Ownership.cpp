//===- Ownership.cpp - Ownership lattice ----------------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Ownership.h"

namespace weavec::core {

std::string_view toString(OwnershipKind kind) noexcept {
  switch (kind) {
  case OwnershipKind::Unknown:
    return "unknown";
  case OwnershipKind::Owned:
    return "owned";
  case OwnershipKind::Shared:
    return "shared";
  case OwnershipKind::Mutable:
    return "mutable";
  case OwnershipKind::Raw:
    return "raw";
  }
  return "<invalid>";
}

} // namespace weavec::core
