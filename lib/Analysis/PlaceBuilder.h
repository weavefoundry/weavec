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
// (allocation, copy, borrow, ...). Pointer arithmetic and pointer-to-pointer
// casts preserve the identity of the object referred to (RFC 0004, *Pointer
// identity*); an integer-to-pointer conversion yields a *raw* value; anything
// else the mapping cannot express is *opaque*: no place and no facts.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H
#define WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H

#include "weavec/Analysis/Summaries.h"
#include "weavec/Core/Moves.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/Raw.h"
#include "weavec/Core/Summary.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

#include "llvm/ADT/DenseMap.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace weavec::analysis {

struct CallEffects;

/// A place denoted by an lvalue expression together with the pointer places
/// that had to be dereferenced to reach it (each of those is *read* by the
/// access, so a moved one is a use-after-free).
struct PlaceRef {
  core::PlaceId place;
  std::vector<core::PlaceId> derefs;
  /// The expression naming each entry of `derefs` (parallel vector; entries
  /// may be null when the dereference was synthesised rather than written).
  std::vector<const clang::Expr *> derefExprs;
  /// The witness in force for each entry of `derefs` (parallel vector): the
  /// element the dereferenced pointer was read from (`a[i]` in `a[i]->x`).
  std::vector<core::ElementWitness> derefElements;
  /// Which element the access named: the nearest subscript on the path
  /// (RFC 0006, *Element witnesses*), `Whole` when there is none, `Unknown`
  /// when a second subscript follows it (`m[i][j]`).
  core::ElementWitness element;

  void addDeref(core::PlaceId pointer, const clang::Expr *expr) {
    derefs.push_back(pointer);
    derefExprs.push_back(expr);
    derefElements.push_back(element);
  }
};

/// How a pointer-typed rvalue was produced.
struct ValueOrigin {
  enum class Kind : std::uint8_t {
    /// A recognised allocation call: fresh owned resource.
    Alloc,
    /// A plain copy of the pointer stored in `place`.
    Copy,
    /// The address of `place` (`&x`, array decay, `&a[i]`, `&p->f`).
    Borrow,
    /// A null pointer constant.
    Null,
    /// `c ? a : b`; see `alternatives`.
    Conditional,
    /// A raw pointer (RFC 0004): cast from an integer, handed out by a
    /// callee as raw, or returned by unchecked code under
    /// `--strict-externs`. See `rawReason`.
    Raw,
    /// Anything else: no facts can be derived.
    Opaque,
  };

  Kind kind = Kind::Opaque;
  /// Copy: the source pointer place. Borrow: the borrowed place.
  std::optional<PlaceRef> place;
  /// Alloc/Raw (callee-produced), or a value returned by a call whose
  /// consumption depends on its result: the call.
  const clang::CallExpr *call = nullptr;
  /// Copy: the value points into the same object as `place` but not
  /// necessarily at the same address (`p + 1`, `strchr(s, c)`). RFC 0006,
  /// *Alias exactness*.
  bool interior = false;
  /// Raw (integer cast): the cast expression, where notes point.
  const clang::Expr *source = nullptr;
  /// Conditional: the two arms.
  std::vector<ValueOrigin> alternatives;
  /// Borrow: whether the borrowed object is `const`-qualified, in which case
  /// the borrow is shared regardless of the destination type.
  bool constObject = false;
  /// Raw: why.
  core::RawReason rawReason = core::RawReason::IntegerCast;
};

class PlaceBuilder {
public:
  PlaceBuilder(core::PlaceTable &table, SummaryStore &summaryStore,
               const clang::ASTContext &astContext)
      : places(table), summaries(summaryStore), context(astContext) {}

  /// The base place for `var`, created on first use.
  core::PlaceId placeForVar(const clang::VarDecl &var);

  /// The base place for `var` if one has been created.
  [[nodiscard]] std::optional<core::PlaceId>
  lookupVar(const clang::VarDecl &var) const;

  /// The place of `member` within the record at `parent`, created on first
  /// use and remembered for `declFor`.
  [[nodiscard]] core::PlaceId fieldPlace(core::PlaceId parent,
                                         const clang::ValueDecl &member);

  /// The variable a base place stands for.
  [[nodiscard]] const clang::VarDecl *varForPlace(core::PlaceId place) const;

  /// Resolves an lvalue expression to a place path, or `std::nullopt` if it
  /// is opaque or not a place at all.
  [[nodiscard]] std::optional<PlaceRef> resolve(const clang::Expr &expr);

  /// The element witness of a subscript expression (RFC 0006): an integer
  /// constant, a variable, or unknown.
  [[nodiscard]] core::ElementWitness witnessOf(const clang::Expr &index);

  /// For a place expression that `resolve` cannot map because its
  /// dereferenced base is not a place (`((T *)(uintptr_t)x)->f`), the
  /// origin of that base if it is a raw value; otherwise `std::nullopt`.
  [[nodiscard]] std::optional<ValueOrigin>
  rawBaseOf(const clang::Expr &placeExpr);

  /// True if the variable or field `place` names is declared `WEAVEC_RAW`
  /// (RFC 0004, *Annotation surface*): every value read from it is raw.
  [[nodiscard]] bool isDeclaredRaw(core::PlaceId place) const;

  /// The declaration `place` names (its variable for a base, its field for
  /// a field step), for locating notes; null when unknown.
  [[nodiscard]] const clang::NamedDecl *declFor(core::PlaceId place) const;

  /// Under `--strict-externs`, a call into code with no summary yields a raw
  /// result rather than an unknown one (RFC 0004, *Boundaries*).
  void setStrictExterns(bool strict) noexcept { strictExterns = strict; }

  /// Resolves an expression yielding a pointer *value* to the place that
  /// pointer is stored in (`p`, `s.p`, `q->next`), looking through parens
  /// and transparent casts.
  [[nodiscard]] std::optional<PlaceRef>
  resolvePointerValue(const clang::Expr &expr);

  /// Classifies a pointer-typed rvalue. Calls are classified through the
  /// callee's summary (RFC 0003): a fresh return is an allocation, a return
  /// of argument `k` is a copy of that argument, and so on.
  [[nodiscard]] ValueOrigin classifyValue(const clang::Expr &expr);

  /// The caller-side place a summary path denotes at `call` (RFC 0003,
  /// *Applying a summary at a call*): `param(i)` is the place holding the
  /// `i`-th argument, `param(i)*` what it points to (or `x` itself when the
  /// argument is `&x`), `global(g)` the global's place. `std::nullopt` when
  /// the argument is not a place.
  [[nodiscard]] std::optional<PlaceRef>
  resolveSummaryPath(const core::SummaryPath &path,
                     const clang::CallExpr &call);

  /// Translates a summary value source at `call` into a caller value
  /// origin. A copy of an argument the callee consumed is reported as a
  /// fresh allocation: ownership went in and came back out.
  [[nodiscard]] ValueOrigin originFromSource(const core::ValueSource &source,
                                             const clang::CallExpr &call,
                                             const core::FunctionSummary &of);

  /// The summary path of a place rooted at a parameter or a global of
  /// `function`, or `std::nullopt` for places rooted at locals.
  [[nodiscard]] std::optional<core::SummaryPath>
  summaryPathOf(core::PlaceId place);

  [[nodiscard]] SummaryStore &summaryStore() noexcept { return summaries; }

  /// True if `expr` is an lvalue expression whose shape the builder
  /// understands (a variable, member, subscript or dereference).
  [[nodiscard]] static bool isPlaceExpr(const clang::Expr &expr);

  /// True if a cast between these two types preserves place identity: every
  /// pointer-to-pointer cast does (RFC 0004, *Pointer identity*); a cast
  /// from or to an integer does not.
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
  SummaryStore &summaries;
  const clang::ASTContext &context;
  bool strictExterns = false;
  llvm::DenseMap<const clang::VarDecl *, core::PlaceId> varPlaces;
  llvm::DenseMap<std::uint32_t, const clang::VarDecl *> placeVars;
  llvm::DenseMap<std::uint32_t, const clang::FieldDecl *> placeFields;
  std::vector<const clang::VarDecl *> order;
};

} // namespace weavec::analysis

#endif // WEAVEC_LIB_ANALYSIS_PLACEBUILDER_H
