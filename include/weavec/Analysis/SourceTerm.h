//===- SourceTerm.h - Quantities spelled over C at a site ------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A quantity as the program could spell it at a site: a constant, `sizeof`,
// a declaration with a member/dereference path, sums, differences and
// products, the length of a string, or a C expression of the program. The
// engine names the symbols of its messages with these (`'b->cap' bytes`),
// and `SiteCollector` spells a library row's terms over a call's arguments.
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_ANALYSIS_SOURCETERM_H
#define WEAVEC_ANALYSIS_SOURCETERM_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"

#include <cstdint>
#include <string>
#include <vector>

namespace weavec::analysis {

/// A quantity spelled over C names at a site.
struct SourceTerm {
  enum class Kind : std::uint8_t {
    Constant,
    /// A declaration and a member/dereference path below it.
    Place,
    Add,
    Sub,
    Mul,
    /// `operands[0] / operands[1]`, the second a positive constant, rounding
    /// down: an element count from a byte extent (§7.4).
    Div,
    /// The length of the string `operands[0]` points to.
    StrLen,
    /// A C expression of the program.
    Expr,
  };

  /// One step below a place: a member (`.f`) or a dereference (`*`).
  /// `p->f` is a dereference followed by a member.
  struct Step {
    enum class Kind : std::uint8_t { Member, Deref };

    Kind kind = Kind::Member;
    /// `Member` only.
    // NOLINTNEXTLINE(readability-redundant-member-init): designated init
    std::string field = {};

    [[nodiscard]] static Step member(std::string name);
    [[nodiscard]] static Step deref();

    friend bool operator==(const Step &, const Step &) = default;
  };

  Kind kind = Kind::Constant;
  std::int64_t constant = 0;
  /// `Place`.
  const clang::ValueDecl *decl = nullptr;
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<Step> path = {};
  /// `Expr`.
  const clang::Expr *expr = nullptr;
  /// `Add`, `Sub`, `Mul`, `Div`: the two sides; `StrLen`: the pointer.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<SourceTerm> operands = {};

  [[nodiscard]] static SourceTerm ofConstant(std::int64_t value);
  [[nodiscard]] static SourceTerm ofPlace(const clang::ValueDecl &decl,
                                          std::vector<Step> path = {});
  [[nodiscard]] static SourceTerm ofExpr(const clang::Expr &expr);
  [[nodiscard]] static SourceTerm add(SourceTerm lhs, SourceTerm rhs);
  [[nodiscard]] static SourceTerm sub(SourceTerm lhs, SourceTerm rhs);
  [[nodiscard]] static SourceTerm mul(SourceTerm lhs, SourceTerm rhs);
  /// `lhs / divisor`, rounding down; `divisor` must be positive.
  [[nodiscard]] static SourceTerm div(SourceTerm lhs, std::int64_t divisor);
  [[nodiscard]] static SourceTerm strLen(SourceTerm pointer);

  /// A debugging spelling: `42`, `n`, `s->len`, `(a + b)`, `(n / 4)`.
  [[nodiscard]] std::string toString() const;
};

} // namespace weavec::analysis

#endif // WEAVEC_ANALYSIS_SOURCETERM_H
