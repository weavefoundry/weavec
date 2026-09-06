//===- AffineSupport.h - Byte sizes and affine sums for the checker -*- C++
//-*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Small helpers shared by the bounds checks of RFC 0011 (Dataflow.cpp) and
// the string checks of RFC 0012 (DataflowStrings.cpp).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_AFFINESUPPORT_H
#define WEAVEC_LIB_ANALYSIS_AFFINESUPPORT_H

#include "weavec/Core/Spatial.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/CharUnits.h"
#include "clang/AST/Type.h"

#include <cstdint>
#include <optional>

namespace weavec::analysis {

/// The size of a complete object type in bytes, or nothing.
[[nodiscard]] inline std::optional<std::int64_t>
byteSizeOf(clang::QualType type, const clang::ASTContext &context) {
  if (type.isNull() || type->isIncompleteType() || type->isFunctionType() ||
      type->isDependentType())
    return std::nullopt;
  const clang::CharUnits size = context.getTypeSizeInChars(type);
  if (size.isZero())
    return std::nullopt;
  return size.getQuantity();
}

/// `a + b` when at most one of them names a place (or both the same).
[[nodiscard]] inline std::optional<core::Affine> sumOf(const core::Affine &a,
                                                       const core::Affine &b) {
  if (a.place && b.place && *a.place != *b.place)
    return std::nullopt;
  core::Affine result = a.place ? a : b;
  if (a.place && b.place) {
    if (__builtin_add_overflow(a.scale, b.scale, &result.scale))
      return std::nullopt;
  }
  if (__builtin_add_overflow(a.constant, b.constant, &result.constant))
    return std::nullopt;
  return result;
}

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_AFFINESUPPORT_H
