//===- Annotations.h - WeaveC source annotations ---------------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// WeaveC annotations are spelled in C as
// `__attribute__((annotate("weavec.X")))` via the macros in
// resources/include/weavec.h. This header recognises them on Clang declarations
// and statements.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_ANNOTATIONS_H
#define WEAVEC_ANALYSIS_ANNOTATIONS_H

#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace weavec::analysis {

/// Annotations understood by WeaveC.
enum class Annotation : std::uint8_t {
  /// `weavec.owned` -- the pointer uniquely owns its referent.
  Owned,
  /// `weavec.borrowed` -- shared, read-only borrow.
  Borrowed,
  /// `weavec.mut_borrowed` -- exclusive, mutable borrow.
  MutBorrowed,
  /// `weavec.unsafe` -- opt out of checking for a function or block.
  Unsafe,
  /// A `weavec.`-prefixed annotation WeaveC does not recognise.
  Invalid,
};

/// Spellings shared with the user-facing `weavec.h` header.
namespace spelling {
inline constexpr llvm::StringLiteral Prefix = "weavec.";
inline constexpr llvm::StringLiteral Owned = "weavec.owned";
inline constexpr llvm::StringLiteral Borrowed = "weavec.borrowed";
inline constexpr llvm::StringLiteral MutBorrowed = "weavec.mut_borrowed";
inline constexpr llvm::StringLiteral Unsafe = "weavec.unsafe";
} // namespace spelling

/// Parses an `annotate` payload. Returns `std::nullopt` for annotations that
/// do not belong to WeaveC (no `weavec.` prefix) and `Annotation::Invalid`
/// for unknown WeaveC annotations.
[[nodiscard]] std::optional<Annotation> parseAnnotation(llvm::StringRef text);

/// The set of WeaveC annotations attached to a declaration.
struct AnnotationSet {
  bool owned = false;
  bool borrowed = false;
  bool mutBorrowed = false;
  bool unsafe = false;
  bool invalid = false;

  [[nodiscard]] bool any() const noexcept {
    return owned || borrowed || mutBorrowed || unsafe || invalid;
  }
};

/// Collects WeaveC annotations from `decl`.
[[nodiscard]] AnnotationSet getAnnotations(const clang::Decl &decl);

/// Returns true if `stmt` is an attributed statement carrying `weavec.unsafe`.
[[nodiscard]] bool isUnsafeBlock(const clang::Stmt &stmt);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_ANNOTATIONS_H
