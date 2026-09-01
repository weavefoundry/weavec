//===- PlaceBuilder.h - Clang expressions to core places -------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Maps Clang lvalue expressions onto structured `core::PlaceId`s (RFC 0002,
// *Places*) and classifies pointer-typed rvalues by how they were produced
// (allocation, copy, borrow, ...). Anything the mapping cannot express
// (pointer arithmetic that may leave an object, casts between unrelated
// pointer types) is *opaque*: it yields no place and no facts.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H
#define WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H

#include "weavec/Core/Place.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include "llvm/ADT/DenseMap.h"

#include <optional>
#include <vector>

namespace weavec::analysis {

/// A place denoted by an lvalue expression together with the pointer places
/// that had to be dereferenced to reach it (each of those is *read* by the
/// access, so a moved one is a use-after-free).
struct PlaceRef {
  core::PlaceId place;
  std::vector<core::PlaceId> derefs;
  /// The expression naming each entry of `derefs` (parallel vector; entries
  /// may be null when the dereference was synthesised rather than written).
  std::vector<const clang::Expr *> derefExprs;

  void addDeref(core::PlaceId pointer, const clang::Expr *expr) {
    derefs.push_back(pointer);
    derefExprs.push_back(expr);
  }
};

/// How a pointer-typed rvalue was produced.
struct ValueOrigin {
  enum class Kind : std::uint8_t {
    /// A recognised allocation call: fresh owned resource.
    Alloc,
    /// `realloc(p, n)`: consumes `place` (the argument) and allocates.
    Realloc,
    /// A plain copy of the pointer stored in `place`.
    Copy,
    /// The address of `place` (`&x`, array decay, `&a[i]`, `&p->f`).
    Borrow,
    /// A null pointer constant.
    Null,
    /// `c ? a : b`; see `alternatives`.
    Conditional,
    /// Anything else: no facts can be derived.
    Opaque,
  };

  Kind kind = Kind::Opaque;
  /// Copy: the source pointer place. Borrow: the borrowed place. Realloc:
  /// the consumed argument.
  std::optional<PlaceRef> place;
  /// Alloc/Realloc: the call.
  const clang::CallExpr *call = nullptr;
  /// Conditional: the two arms.
  std::vector<ValueOrigin> alternatives;
  /// Borrow: whether the borrowed object is `const`-qualified, in which case
  /// the borrow is shared regardless of the destination type.
  bool constObject = false;
};

class PlaceBuilder {
public:
  explicit PlaceBuilder(core::PlaceTable &table) : places(table) {}

  /// The base place for `var`, created on first use.
  core::PlaceId placeForVar(const clang::VarDecl &var);

  /// The base place for `var` if one has been created.
  [[nodiscard]] std::optional<core::PlaceId>
  lookupVar(const clang::VarDecl &var) const;

  /// The variable a base place stands for.
  [[nodiscard]] const clang::VarDecl *varForPlace(core::PlaceId place) const;

  /// Resolves an lvalue expression to a place path, or `std::nullopt` if it
  /// is opaque or not a place at all.
  [[nodiscard]] std::optional<PlaceRef> resolve(const clang::Expr &expr);

  /// Resolves an expression yielding a pointer *value* to the place that
  /// pointer is stored in (`p`, `s.p`, `q->next`), looking through parens
  /// and transparent casts.
  [[nodiscard]] std::optional<PlaceRef>
  resolvePointerValue(const clang::Expr &expr);

  /// Classifies a pointer-typed rvalue.
  [[nodiscard]] ValueOrigin classifyValue(const clang::Expr &expr);

  /// True if `expr` is an lvalue expression whose shape the builder
  /// understands (a variable, member, subscript or dereference).
  [[nodiscard]] static bool isPlaceExpr(const clang::Expr &expr);

  /// True if a cast between these two types preserves place identity.
  /// `void *` and byte pointers are how C spells generic pointers, so casts
  /// to and from them are transparent; other pointer-to-pointer casts are
  /// opaque (RFC 0002).
  [[nodiscard]] static bool isTransparentCast(clang::QualType from,
                                              clang::QualType to);

  /// Strips parens and transparent casts (implicit or explicit).
  [[nodiscard]] static const clang::Expr &
  stripTransparent(const clang::Expr &expr);

  [[nodiscard]] core::PlaceTable &table() noexcept { return places; }

  /// Variables in the order their places were created.
  [[nodiscard]] const std::vector<const clang::VarDecl *> &
  variables() const noexcept {
    return order;
  }

private:
  core::PlaceTable &places;
  llvm::DenseMap<const clang::VarDecl *, core::PlaceId> varPlaces;
  std::vector<const clang::VarDecl *> order;
};

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H
