//===- CheckPlan.cpp - Planned runtime checks (RFC 0030) ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/CheckPlan.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace weavec::core {

using Template = CheckPlanEntry::Template;
using Form = CheckPlanEntry::Form;
using Placement = CheckPlanEntry::Placement;

CheckPathStep CheckPathStep::member(std::string name) {
  return CheckPathStep{.kind = Kind::Member, .field = std::move(name)};
}

CheckPathStep CheckPathStep::deref() {
  return CheckPathStep{.kind = Kind::Deref};
}

CheckTerm CheckTerm::ofConstant(std::int64_t value) {
  return CheckTerm{.kind = Kind::Constant, .constant = value};
}

CheckTerm CheckTerm::ofPlace(std::uint64_t handle,
                             std::vector<CheckPathStep> path) {
  return CheckTerm{
      .kind = Kind::Place, .handle = handle, .path = std::move(path)};
}

CheckTerm CheckTerm::sizeOf(std::uint64_t type) {
  return CheckTerm{.kind = Kind::SizeOf, .handle = type};
}

static CheckTerm binary(CheckTerm::Kind kind, CheckTerm lhs, CheckTerm rhs) {
  CheckTerm term{.kind = kind};
  term.operands.push_back(std::move(lhs));
  term.operands.push_back(std::move(rhs));
  return term;
}

CheckTerm CheckTerm::add(CheckTerm lhs, CheckTerm rhs) {
  return binary(Kind::Add, std::move(lhs), std::move(rhs));
}

CheckTerm CheckTerm::sub(CheckTerm lhs, CheckTerm rhs) {
  return binary(Kind::Sub, std::move(lhs), std::move(rhs));
}

CheckTerm CheckTerm::mul(CheckTerm lhs, CheckTerm rhs) {
  return binary(Kind::Mul, std::move(lhs), std::move(rhs));
}

CheckTerm CheckTerm::div(CheckTerm lhs, std::int64_t divisor) {
  return binary(Kind::Div, std::move(lhs), ofConstant(divisor));
}

CheckTerm CheckTerm::strnlen(CheckTerm pointer, CheckTerm bound) {
  return binary(Kind::StrNLen, std::move(pointer), std::move(bound));
}

bool CheckTerm::isWellFormed() const noexcept {
  switch (kind) {
  case Kind::Constant:
  case Kind::SizeOf:
    return path.empty() && operands.empty();
  case Kind::Place:
    return operands.empty() &&
           std::ranges::all_of(path, [](const CheckPathStep &step) {
             return (step.kind == CheckPathStep::Kind::Member) ==
                    !step.field.empty();
           });
  case Kind::Add:
  case Kind::Sub:
  case Kind::Mul:
  case Kind::StrNLen:
    return path.empty() && operands.size() == 2 && operands[0].isWellFormed() &&
           operands[1].isWellFormed();
  case Kind::Div:
    return path.empty() && operands.size() == 2 && operands[0].isWellFormed() &&
           operands[1].kind == Kind::Constant && operands[1].constant > 0;
  }
  return false;
}

/// `$3`, `*$3`, `$3->len`, `$3.hdr.len`, `*$3->next`.
static std::string placeText(std::uint64_t handle,
                             const std::vector<CheckPathStep> &path) {
  std::string text = "$" + std::to_string(handle);
  bool pendingDeref = false;
  for (const CheckPathStep &step : path) {
    if (step.kind == CheckPathStep::Kind::Deref) {
      if (pendingDeref)
        text.insert(0, 1, '*');
      pendingDeref = true;
      continue;
    }
    text += pendingDeref ? "->" : ".";
    text += step.field;
    pendingDeref = false;
  }
  if (pendingDeref)
    text.insert(0, 1, '*');
  return text;
}

/// The infix spelling of an arithmetic term.
static std::string_view operatorText(CheckTerm::Kind kind) noexcept {
  switch (kind) {
  case CheckTerm::Kind::Add:
    return " + ";
  case CheckTerm::Kind::Sub:
    return " - ";
  case CheckTerm::Kind::Mul:
    return " * ";
  case CheckTerm::Kind::Div:
    return " / ";
  case CheckTerm::Kind::Constant:
  case CheckTerm::Kind::Place:
  case CheckTerm::Kind::SizeOf:
  case CheckTerm::Kind::StrNLen:
    break;
  }
  return " ? ";
}

std::string CheckTerm::toString() const {
  switch (kind) {
  case Kind::Constant:
    return std::to_string(constant);
  case Kind::Place:
    return placeText(handle, path);
  case Kind::SizeOf:
    return "sizeof(#" + std::to_string(handle) + ")";
  case Kind::Add:
  case Kind::Sub:
  case Kind::Mul:
  case Kind::Div: {
    if (operands.size() != 2)
      return "<malformed>";
    std::string text = "(";
    text += operands[0].toString();
    text += operatorText(kind);
    text += operands[1].toString();
    text += ')';
    return text;
  }
  case Kind::StrNLen:
    if (operands.size() != 2)
      return "<malformed>";
    return "strnlen(" + operands[0].toString() + ", " + operands[1].toString() +
           ")";
  }
  return "<malformed>";
}

std::string_view toString(Template kind) noexcept {
  switch (kind) {
  case Template::Nonnull:
    return "nonnull";
  case Template::Index:
    return "index";
  case Template::Span:
    return "span";
  case Template::Len:
    return "len";
  case Template::Disjoint:
    return "disjoint";
  case Template::Assert:
    return "assert";
  }
  return "<invalid>";
}

std::string_view toString(Form form) noexcept {
  switch (form) {
  case Form::Plain:
    return "plain";
  case Form::IfNonZero:
    return "if-non-zero";
  case Form::Function:
    return "function";
  case Form::Result:
    return "result";
  case Form::Violation:
    return "violation";
  }
  return "<invalid>";
}

std::string_view toString(Placement placement) noexcept {
  switch (placement) {
  case Placement::WrapOperand:
    return "wrap-operand";
  case Placement::WrapIndex:
    return "wrap-index";
  case Placement::WrapArgument:
    return "wrap-argument";
  case Placement::ReplaceAccess:
    return "replace-access";
  case Placement::BeforeCall:
    return "before-call";
  case Placement::ReplaceCall:
    return "replace-call";
  }
  return "<invalid>";
}

std::optional<std::size_t> operandCount(Template kind, Form form,
                                        Placement placement) noexcept {
  if (form == Form::Violation)
    return 0;
  switch (kind) {
  case Template::Nonnull:
    if (form == Form::Plain && (placement == Placement::WrapOperand ||
                                placement == Placement::WrapArgument))
      return 0;
    if (form == Form::IfNonZero && placement == Placement::WrapArgument)
      return 1;
    if (form == Form::Function && placement == Placement::WrapOperand)
      return 0;
    return std::nullopt;
  case Template::Index:
    if (form == Form::Plain && (placement == Placement::WrapIndex ||
                                placement == Placement::WrapOperand))
      return 1;
    return std::nullopt;
  case Template::Span:
    if (form == Form::Plain && (placement == Placement::ReplaceAccess ||
                                placement == Placement::WrapOperand ||
                                placement == Placement::WrapArgument))
      return 3;
    return std::nullopt;
  case Template::Len:
    if (form == Form::Plain && placement == Placement::WrapArgument)
      return 1;
    if (form == Form::Plain && placement == Placement::BeforeCall)
      return 2;
    if (form == Form::Result && placement == Placement::ReplaceCall)
      return 1;
    return std::nullopt;
  case Template::Disjoint:
    if (form == Form::Plain && placement == Placement::WrapArgument)
      return 2;
    return std::nullopt;
  case Template::Assert:
    if (form == Form::Plain && placement == Placement::ReplaceCall)
      return 0;
    return std::nullopt;
  }
  return std::nullopt;
}

bool isWellFormed(const CheckPlanEntry &entry) noexcept {
  const auto count = operandCount(entry.kind, entry.form, entry.placement);
  if (!count || *count != entry.operands.size())
    return false;
  if (entry.guard && !entry.guard->isWellFormed())
    return false;
  return std::ranges::all_of(entry.operands, [](const CheckTerm &term) {
    return term.isWellFormed();
  });
}

CheckTemplate ledgerTemplate(const CheckPlanEntry &entry) noexcept {
  if (entry.form == Form::Violation)
    return CheckTemplate::Violation;
  switch (entry.kind) {
  case Template::Nonnull:
    return CheckTemplate::Nonnull;
  case Template::Index:
    return CheckTemplate::Index;
  case Template::Span:
    return CheckTemplate::Span;
  case Template::Len:
    return CheckTemplate::Len;
  case Template::Disjoint:
    return CheckTemplate::Disjoint;
  case Template::Assert:
    return CheckTemplate::Assert;
  }
  return CheckTemplate::Violation;
}

FacetCheck facetCheck(const CheckPlanEntry &entry) noexcept {
  return FacetCheck{.kind = ledgerTemplate(entry), .proven = entry.proven};
}

std::string helperName(const CheckPlanEntry &entry) {
  if (entry.form == Form::Violation)
    return {};
  std::string name = entry.proven ? "__weavec_prv_" : "__weavec_chk_";
  name += toString(entry.kind);
  switch (entry.form) {
  case Form::IfNonZero:
    name += "_n";
    break;
  case Form::Function:
    name += "_fn";
    break;
  case Form::Result:
    name += "_r";
    break;
  case Form::Plain:
  case Form::Violation:
    break;
  }
  return name;
}

/// §10.4: on one operand `nonnull` is innermost, then `index`.
static int nestingRank(Template kind) noexcept {
  switch (kind) {
  case Template::Nonnull:
    return 0;
  case Template::Index:
    return 1;
  case Template::Span:
  case Template::Len:
  case Template::Disjoint:
  case Template::Assert:
    return 2;
  }
  return 2;
}

void CheckPlan::sort() {
  std::ranges::stable_sort(entries, [](const CheckPlanEntry &a,
                                       const CheckPlanEntry &b) {
    return std::make_tuple(a.site, a.facet, a.requirement, nestingRank(a.kind),
                           a.kind, a.form, a.placement, a.argument) <
           std::make_tuple(b.site, b.facet, b.requirement, nestingRank(b.kind),
                           b.kind, b.form, b.placement, b.argument);
  });
}

std::vector<const CheckPlanEntry *> CheckPlan::entriesFor(SiteId site) const {
  std::vector<const CheckPlanEntry *> found;
  for (const CheckPlanEntry &entry : entries) {
    if (entry.site == site)
      found.push_back(&entry);
  }
  return found;
}

} // namespace weavec::core
