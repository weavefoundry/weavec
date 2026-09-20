//===- CheckWitness.h - What a check needs (RFC 0030) -----------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §14: a *witness* tells `CheckPlanner` what the check of one facet
// (or of one requirement record of it) would compare: the extent and whether
// it is exact, declared or a lower bound; the object base and the element
// width of a cursor access; the lengths of a library call; and the two
// facts of §10.3 only the engine can establish (rule 4, the terms' places
// are unmodified since the extent was derived; rule 5, the terms' own
// accesses are proven or trusted at the site). The engine publishes
// witnesses through `LedgerAdapter::witness`; `SiteCollector` builds the
// witness of a spatial facet whose §2.6 default is `checked` from the
// declarations alone.
//
// Terms name C entities at the site: constants, `sizeof`, a declaration with
// a member/dereference path, sums, differences and products, the bounded
// length of a string, or a C expression of the program that the planner
// examines. `CheckPlanner` decides whether each term is expressible and
// turns it into a `core::CheckTerm` over opaque handles.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_CHECKWITNESS_H
#define WEAVEC_ANALYSIS_CHECKWITNESS_H

#include "weavec/Core/CheckPlan.h"
#include "weavec/Core/PointerKind.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace weavec::analysis {

/// An extra operand of a check, over C names at the site (§10.1).
struct WitnessTerm {
  enum class Kind : std::uint8_t {
    Constant,
    /// A declaration and a member/dereference path below it.
    Place,
    /// `sizeof(type)`.
    SizeOf,
    Add,
    Sub,
    Mul,
    /// `operands[0] / operands[1]`, the second a positive constant, rounding
    /// down: an element count from a byte extent (§7.4).
    Div,
    /// The length of the string `operands[0]` points to; the planner bounds
    /// it by the check's `have` (`__weavec_strnlen`, §10.3 rule 2).
    StrLen,
    /// A C expression of the program, evaluated again by the check.
    Expr,
  };

  Kind kind = Kind::Constant;
  std::int64_t constant = 0;
  /// `Place`.
  const clang::ValueDecl *decl = nullptr;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<core::CheckPathStep> path = {};
  /// `SizeOf`.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  clang::QualType type = {};
  /// `Expr`.
  const clang::Expr *expr = nullptr;
  /// `Add`, `Sub`, `Mul`, `Div`: the two sides; `StrLen`: the pointer.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<WitnessTerm> operands = {};

  [[nodiscard]] static WitnessTerm ofConstant(std::int64_t value);
  [[nodiscard]] static WitnessTerm
  ofPlace(const clang::ValueDecl &decl,
          std::vector<core::CheckPathStep> path = {});
  [[nodiscard]] static WitnessTerm sizeOf(clang::QualType type);
  [[nodiscard]] static WitnessTerm ofExpr(const clang::Expr &expr);
  [[nodiscard]] static WitnessTerm add(WitnessTerm lhs, WitnessTerm rhs);
  [[nodiscard]] static WitnessTerm sub(WitnessTerm lhs, WitnessTerm rhs);
  [[nodiscard]] static WitnessTerm mul(WitnessTerm lhs, WitnessTerm rhs);
  /// `lhs / divisor`, rounding down; `divisor` must be positive.
  [[nodiscard]] static WitnessTerm div(WitnessTerm lhs, std::int64_t divisor);
  [[nodiscard]] static WitnessTerm strLen(WitnessTerm pointer);

  /// A debugging spelling: `42`, `n`, `s->len`, `sizeof(int)`, `(a + b)`,
  /// `(n / 4)`.
  [[nodiscard]] std::string toString() const;
};

/// What the check of one facet, or of one requirement record, compares.
struct CheckWitness {
  enum class Shape : std::uint8_t {
    /// `index(i, n)`: the subscript (0 for a dereference) stays below
    /// `extent` elements.
    Index,
    /// `span(p, i, base, bytes, width)`: the `width` bytes accessed stay in
    /// `[base, base + extent)`, with `extent` in bytes.
    Span,
    /// `len(need, have)`: `need` does not exceed `extent` (the have).
    Length,
    /// `disjoint(d, s, n)`: the first `need` bytes behind the argument and
    /// behind `other` do not overlap.
    Disjoint,
  };

  Shape shape = Shape::Index;
  /// The requirement record of the facet the witness is for; none for the
  /// facet as a whole.
  std::optional<std::uint16_t> requirement = std::nullopt;
  /// For an argument of a call-like site: which argument the check wraps.
  std::optional<std::uint8_t> argument = std::nullopt;
  /// The count (`Index`), byte extent (`Span`) or have (`Length`).
  std::optional<WitnessTerm> extent = std::nullopt;
  /// §10.3 rule 8: only exact extents and declared kinds may be compared
  /// against.
  core::ExtentClass extentClass = core::ExtentClass::LowerBound;
  /// `Span`: the start of the object and the width of one access.
  std::optional<WitnessTerm> base = std::nullopt;
  std::optional<WitnessTerm> width = std::nullopt;
  /// The pointer's offset from `base`, or the index, when known.
  std::optional<WitnessTerm> offset = std::nullopt;
  /// `Length`, `Disjoint`: the needed length in bytes.
  std::optional<WitnessTerm> need = std::nullopt;
  /// `Disjoint`: the other pointer.
  std::optional<WitnessTerm> other = std::nullopt;
  /// §10.3 rule 4: every place the terms name still holds, at the site, the
  /// value the extent was derived from.
  bool unmodified = false;
  /// §10.3 rule 5: every memory access the terms make is proven or trusted
  /// at the site.
  bool accessesSafe = false;
  /// §7.5, §10.4: the check binds only when this term (a requirement's
  /// guard, non-zero exactly when it holds) is non-zero; none for always.
  std::optional<WitnessTerm> guard = std::nullopt;
  /// §7.6 *Application*: the pointer was loaded from this field of this
  /// object.
  const clang::FieldDecl *loadedField = nullptr;
  const clang::Expr *loadedFrom = nullptr;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_CHECKWITNESS_H
