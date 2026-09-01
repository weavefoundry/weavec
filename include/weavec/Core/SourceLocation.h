//===- SourceLocation.h - Frontend-neutral source positions ----*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_SOURCELOCATION_H
#define WEAVEC_CORE_SOURCELOCATION_H

#include <cstdint>
#include <string>

namespace weavec::core {

/// A resolved source position, independent of any particular frontend.
///
/// The Clang integration layer converts `clang::SourceLocation` into this
/// representation before handing facts to the core model, and maps back when
/// emitting diagnostics through Clang.
struct SourceLocation {
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;

  /// An opaque frontend-specific handle (e.g. the raw encoding of a
  /// `clang::SourceLocation`) that lets the frontend recover its native
  /// location precisely. Zero means "none".
  std::uint64_t opaque = 0;

  [[nodiscard]] bool isValid() const noexcept { return line != 0; }

  /// Formats as `file:line:column`.
  [[nodiscard]] std::string toString() const;

  friend bool operator==(const SourceLocation &,
                         const SourceLocation &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_SOURCELOCATION_H
