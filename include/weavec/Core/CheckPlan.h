//===- CheckPlan.h - Planned runtime checks (RFC 0030) ---------*- C++ -*-===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §10.1: `CheckPlanner` turns each checked requirement record of the
// ledger (§2.5), and each lowered violation (§3.4), into one `CheckPlanEntry`.
// The plan is pure data. It is computed in every mode, before the
// require-level errors and the summary line, and `CheckEmitter` (Frontend)
// applies it through Sema when checks are enabled.
//
// Operands that name program state are *place handles*: opaque integers the
// Analysis layer resolves to a `clang::ValueDecl` plus a member/dereference
// path. `sizeof` operands name an opaque type id the same way. Core never
// sees Clang.
//
// Only the template's *extra* operands are terms. The expression a rewrite
// wraps in place (the dereferenced pointer, the subscript, the call argument)
// is evaluated exactly once by the rewrite and is not an operand (§10.3,
// §10.4).
//
//===----------------------------------------------------------------------===//

#ifndef WEAVEC_CORE_CHECKPLAN_H
#define WEAVEC_CORE_CHECKPLAN_H

#include "weavec/Core/Ledger.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace weavec::core {

/// One step below a place handle: a member (`.f`) or a dereference (`*`).
/// `p->f` is a dereference followed by a member.
struct CheckPathStep {
  enum class Kind : std::uint8_t { Member, Deref };

  Kind kind = Kind::Member;
  /// `Member` only.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::string field = {};

  [[nodiscard]] static CheckPathStep member(std::string name);
  [[nodiscard]] static CheckPathStep deref();

  friend bool operator==(const CheckPathStep &,
                         const CheckPathStep &) = default;
  friend std::strong_ordering operator<=>(const CheckPathStep &,
                                          const CheckPathStep &) = default;
};

/// §10.1: an extra operand of a check.
///
///   constant | place | sizeof(type) | a + b | a - b | a * b | a / k
///   | strnlen(pointer, bound)
///
/// `+`, `-` and `*` are evaluated by the prelude's term helpers over 64-bit
/// integers, saturating in the direction that fails closed (§10.2), never
/// by C's own arithmetic. `a / k` divides by a positive constant, rounding
/// down: it only turns a byte extent into an element count (§7.4:
/// `index(i, bytes / 4)` for an allocation of `bytes` bytes whose element
/// count the facts do not give), where rounding down fails closed and
/// nothing can overflow.
struct CheckTerm {
  enum class Kind : std::uint8_t {
    Constant,
    Place,
    SizeOf,
    Add,
    Sub,
    Mul,
    /// `operands[0] / operands[1]`, the second a positive constant.
    Div,
    StrNLen,
  };

  Kind kind = Kind::Constant;
  /// `Constant`.
  std::int64_t constant = 0;
  /// `Place`: the opaque place handle; `SizeOf`: the opaque type id.
  std::uint64_t handle = 0;
  /// `Place`: the member and dereference steps below the handle.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<CheckPathStep> path = {};
  /// `Add`, `Sub`, `Mul`, `Div`: the two sides; `StrNLen`: the pointer and
  /// the bound.
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<CheckTerm> operands = {};

  [[nodiscard]] static CheckTerm ofConstant(std::int64_t value);
  [[nodiscard]] static CheckTerm ofPlace(std::uint64_t handle,
                                         std::vector<CheckPathStep> path = {});
  [[nodiscard]] static CheckTerm sizeOf(std::uint64_t type);
  [[nodiscard]] static CheckTerm add(CheckTerm lhs, CheckTerm rhs);
  [[nodiscard]] static CheckTerm sub(CheckTerm lhs, CheckTerm rhs);
  [[nodiscard]] static CheckTerm mul(CheckTerm lhs, CheckTerm rhs);
  /// `lhs / divisor`, rounding down; `divisor` must be positive.
  [[nodiscard]] static CheckTerm div(CheckTerm lhs, std::int64_t divisor);
  [[nodiscard]] static CheckTerm strnlen(CheckTerm pointer, CheckTerm bound);

  /// Each kind has exactly the fields and operand count it uses.
  [[nodiscard]] bool isWellFormed() const noexcept;
  /// A debugging spelling: `42`, `$3->len`, `*$3`, `sizeof(#7)`,
  /// `($1 + 4)`, `($1 / 4)`, `strnlen($2, 16)`.
  [[nodiscard]] std::string toString() const;

  friend bool operator==(const CheckTerm &, const CheckTerm &) = default;
};

/// §10.1: one planned check for one requirement record of one facet.
struct CheckPlanEntry {
  /// The six templates of §10.2.
  enum class Template : std::uint8_t {
    Nonnull,
    Index,
    Span,
    Len,
    Disjoint,
    Assert,
  };
  /// `IfNonZero`: `nonnull` for `null-if-zero` arguments; `Function`:
  /// `nonnull` for the callee operand of an indirect call; `Result`: `len`
  /// over the `snprintf` lowering of `sprintf`; `Violation`: the
  /// unconditional trap of a lowered violation, of any template (§3.4).
  enum class Form : std::uint8_t {
    Plain,
    IfNonZero,
    Function,
    Result,
    Violation
  };
  /// §10.4: where the rewrite goes.
  enum class Placement : std::uint8_t {
    WrapOperand,
    WrapIndex,
    WrapArgument,
    ReplaceAccess,
    BeforeCall,
    ReplaceCall,
  };

  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  SiteId site = {};
  Facet facet = Facet::Null;
  /// Index into the facet row's `requirements`; 0 and unused for facets
  /// without requirement records.
  std::uint16_t requirement = 0;
  Template kind = Template::Nonnull;
  Form form = Form::Plain;
  Placement placement = Placement::WrapOperand;
  /// `WrapArgument`: the argument's 0-based index.
  std::uint8_t argument = 0;
  /// The template's extra operands, in order (`operandCount`).
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<CheckTerm> operands = {};
  /// §7.5 requirement guard: the check runs only when the guard is non-zero.
  /// A guard `c < e` is the term `e - c`, which the term helpers saturate at
  /// zero.
  std::optional<CheckTerm> guard = std::nullopt;
  /// Verify mode: a check of a proven facet (`__weavec_prv_*`, §10.7).
  bool proven = false;

  friend bool operator==(const CheckPlanEntry &,
                         const CheckPlanEntry &) = default;
};

[[nodiscard]] std::string_view toString(CheckPlanEntry::Template kind) noexcept;
[[nodiscard]] std::string_view toString(CheckPlanEntry::Form form) noexcept;
[[nodiscard]] std::string_view
toString(CheckPlanEntry::Placement placement) noexcept;

/// The number of extra operands a template takes in a form and placement,
/// or none when §10.2–§10.4 give that combination no meaning:
///
///   nonnull   Plain     WrapOperand, WrapArgument        -
///   nonnull   IfNonZero WrapArgument                     n
///   nonnull   Function  WrapOperand                      -
///   index     Plain     WrapIndex, WrapOperand (i = 0)   n
///   span      Plain     ReplaceAccess, WrapOperand,      base, bytes, width
///                       WrapArgument
///   len       Plain     WrapArgument (need wrapped)      have
///   len       Plain     BeforeCall                       need, have
///   len       Result    ReplaceCall                      have
///   disjoint  Plain     WrapArgument (d wrapped)         s, n
///   assert    Plain     ReplaceCall                      -
///   any       Violation any                              -
[[nodiscard]] std::optional<std::size_t>
operandCount(CheckPlanEntry::Template kind, CheckPlanEntry::Form form,
             CheckPlanEntry::Placement placement) noexcept;

/// The combination is meaningful, the operand count matches, and every term
/// (and the guard) is well formed.
[[nodiscard]] bool isWellFormed(const CheckPlanEntry &entry) noexcept;

/// The ledger's `check.template` for an entry: the template, or `violation`
/// for the `Violation` form.
[[nodiscard]] CheckTemplate
ledgerTemplate(const CheckPlanEntry &entry) noexcept;
/// The ledger's `check` object for an entry.
[[nodiscard]] FacetCheck facetCheck(const CheckPlanEntry &entry) noexcept;

/// The prelude helper the entry calls (§10.2): `__weavec_chk_<template>`
/// with the `_n`, `_fn` and `_r` suffixes of its form, or `__weavec_prv_*`
/// for a verify check of a proven facet. Empty for the `Violation` form,
/// which is an inline `__builtin_verbose_trap`.
[[nodiscard]] std::string helperName(const CheckPlanEntry &entry);

/// The planned checks of one unit.
struct CheckPlan {
  // NOLINTNEXTLINE(readability-redundant-member-init): designated-init default
  std::vector<CheckPlanEntry> entries = {};

  void add(CheckPlanEntry entry) { entries.push_back(std::move(entry)); }
  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }

  /// Puts the entries in the canonical order: by site, facet and
  /// requirement, then (on one operand, §10.4) `nonnull` innermost before
  /// `index`, then template, form and placement. Stable otherwise.
  void sort();
  /// The entries for one site, in plan order.
  [[nodiscard]] std::vector<const CheckPlanEntry *>
  entriesFor(SiteId site) const;

  friend bool operator==(const CheckPlan &, const CheckPlan &) = default;
};

} // namespace weavec::core

#endif // WEAVEC_CORE_CHECKPLAN_H
