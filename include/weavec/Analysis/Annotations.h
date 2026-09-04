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

#include "weavec/Core/Ownership.h"

#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace weavec::analysis {

/// Annotations understood by WeaveC.
enum class Annotation : std::uint8_t {
  /// `weavec.owned` -- the pointer uniquely owns its referent.
  Owned,
  /// `weavec.borrowed` -- shared, read-only borrow.
  Borrowed,
  /// `weavec.mut_borrowed` -- exclusive, mutable borrow.
  MutBorrowed,
  /// `weavec.raw` -- no ownership guarantee; dereferencing or releasing the
  /// pointer requires an unsafe region (RFC 0004).
  Raw,
  /// `weavec.unsafe` -- the function body or block is an unsafe region.
  Unsafe,
  /// `weavec.nullable` -- the pointer may be null (RFC 0008).
  Nullable,
  /// `weavec.nonnull` -- the pointer is never null (RFC 0008).
  NonNull,
  /// `weavec.retains` -- the callee takes a reference on the argument's
  /// object: the caller's place gains a share (RFC 0010).
  Retains,
  /// `weavec.releases` -- the callee releases one reference: the argument's
  /// name is dead afterwards, other shares are untouched (RFC 0010).
  Releases,
  /// `weavec.refcount` -- the integer field is a reference count (RFC 0010).
  Refcount,
  /// `weavec.family.<f>` -- the release family of a `WEAVEC_OWNED` pointer
  /// (RFC 0010, closing RFC 0007's open item). The family is carried in
  /// `AnnotationSet::family`.
  Family,
  /// A `weavec.`-prefixed annotation WeaveC does not recognise.
  Invalid,
};

/// Spellings shared with the user-facing `weavec.h` header.
namespace spelling {
inline constexpr llvm::StringLiteral Prefix = "weavec.";
inline constexpr llvm::StringLiteral Owned = "weavec.owned";
inline constexpr llvm::StringLiteral Borrowed = "weavec.borrowed";
inline constexpr llvm::StringLiteral MutBorrowed = "weavec.mut_borrowed";
inline constexpr llvm::StringLiteral Raw = "weavec.raw";
inline constexpr llvm::StringLiteral Unsafe = "weavec.unsafe";
inline constexpr llvm::StringLiteral Nullable = "weavec.nullable";
inline constexpr llvm::StringLiteral NonNull = "weavec.nonnull";
inline constexpr llvm::StringLiteral Retains = "weavec.retains";
inline constexpr llvm::StringLiteral Releases = "weavec.releases";
inline constexpr llvm::StringLiteral Refcount = "weavec.refcount";
/// `weavec.family.<f>`: the prefix, followed by the family name.
inline constexpr llvm::StringLiteral FamilyPrefix = "weavec.family.";
} // namespace spelling

/// Parses an `annotate` payload. Returns `std::nullopt` for annotations that
/// do not belong to WeaveC (no `weavec.` prefix) and `Annotation::Invalid`
/// for unknown WeaveC annotations. A `weavec.family.<f>` payload with an
/// empty or malformed `<f>` is `Invalid`.
[[nodiscard]] std::optional<Annotation> parseAnnotation(llvm::StringRef text);

/// The set of WeaveC annotations attached to a declaration.
struct AnnotationSet {
  bool owned = false;
  bool borrowed = false;
  bool mutBorrowed = false;
  bool raw = false;
  bool unsafe = false;
  bool nullable = false;
  bool nonNull = false;
  /// RFC 0010.
  bool retains = false;
  bool releases = false;
  bool refcount = false;
  bool invalid = false;
  /// RFC 0010: the release family `WEAVEC_OWNED_BY(f)` names; empty when
  /// there is none. Two different families on one declaration are
  /// `invalid`.
  std::string family;

  [[nodiscard]] bool any() const noexcept {
    return owned || borrowed || mutBorrowed || raw || unsafe || nullable ||
           nonNull || retains || releases || refcount || invalid ||
           !family.empty();
  }
  /// True if the set says something about nullness (RFC 0008).
  [[nodiscard]] bool nullness() const noexcept { return nullable || nonNull; }
  /// True if the set says something about ownership (`owned`, `borrowed`,
  /// `mutBorrowed`, `raw`, `retains` or `releases`), as opposed to
  /// `unsafe`/`invalid`.
  [[nodiscard]] bool ownership() const noexcept {
    return owned || borrowed || mutBorrowed || raw || retains || releases;
  }
  /// The kind a raw pointer may be *asserted* into (RFC 0004, *Laundering*):
  /// the declared ownership kind, if it is anything but `raw`.
  [[nodiscard]] std::optional<core::OwnershipKind> safeKind() const noexcept {
    if (owned)
      return core::OwnershipKind::Owned;
    if (mutBorrowed)
      return core::OwnershipKind::Mutable;
    if (borrowed)
      return core::OwnershipKind::Shared;
    return std::nullopt;
  }

  /// Merges `other` into this set.
  void merge(const AnnotationSet &other);
};

/// Collects WeaveC annotations from `decl`.
[[nodiscard]] AnnotationSet getAnnotations(const clang::Decl &decl);

/// Returns true if `stmt` is an attributed statement carrying `weavec.unsafe`.
[[nodiscard]] bool isUnsafeBlock(const clang::Stmt &stmt);

/// The macro spelling for the ownership annotation in `set`, if exactly one
/// of `WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW` applies
/// (the first in that order wins otherwise); null if none does.
[[nodiscard]] const char *macroSpelling(const AnnotationSet &set) noexcept;

/// Ownership annotations written on a function *type* (RFC 0004, *Signatures
/// for function pointers*): those on the parameters of the prototype in a
/// declarator of function-pointer type, and, as the result's annotations,
/// those on the declarator itself and on any `typedef` passed through on
/// the way to the prototype.
struct FunctionTypeAnnotations {
  AnnotationSet result;
  std::vector<AnnotationSet> params;
  /// The prototype the annotations were read from, or null if the type is
  /// not a (pointer to) prototyped function.
  const clang::FunctionProtoType *prototype = nullptr;

  [[nodiscard]] bool anyOwnership() const noexcept;
};

/// Reads the function-type annotations reachable from `decl`'s written type.
/// `decl` is the variable, parameter, field or typedef the callee expression
/// of an indirect call names.
[[nodiscard]] FunctionTypeAnnotations
collectFunctionTypeAnnotations(const clang::Decl &decl);

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_ANNOTATIONS_H
