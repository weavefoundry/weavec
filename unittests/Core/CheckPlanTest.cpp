//===- CheckPlanTest.cpp - Tests for RFC 0030 check plans -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/CheckPlan.h"

#include <gtest/gtest.h>

#include <vector>

namespace weavec::core {

using Template = CheckPlanEntry::Template;
using Form = CheckPlanEntry::Form;
using Placement = CheckPlanEntry::Placement;

TEST(CheckPlan, TermsSpellTheirStructure) {
  EXPECT_EQ(CheckTerm::ofConstant(42).toString(), "42");
  EXPECT_EQ(CheckTerm::ofPlace(3).toString(), "$3");
  EXPECT_EQ(CheckTerm::ofPlace(
                3, {CheckPathStep::deref(), CheckPathStep::member("len")})
                .toString(),
            "$3->len");
  EXPECT_EQ(CheckTerm::ofPlace(3, {CheckPathStep::deref()}).toString(), "*$3");
  EXPECT_EQ(CheckTerm::ofPlace(
                3, {CheckPathStep::member("hdr"), CheckPathStep::member("len")})
                .toString(),
            "$3.hdr.len");
  EXPECT_EQ(CheckTerm::ofPlace(3, {CheckPathStep::deref(),
                                   CheckPathStep::member("next"),
                                   CheckPathStep::deref()})
                .toString(),
            "*$3->next");
  EXPECT_EQ(CheckTerm::sizeOf(7).toString(), "sizeof(#7)");
  EXPECT_EQ(CheckTerm::mul(
                CheckTerm::add(CheckTerm::ofPlace(1), CheckTerm::ofConstant(4)),
                CheckTerm::sizeOf(7))
                .toString(),
            "(($1 + 4) * sizeof(#7))");
  EXPECT_EQ(CheckTerm::sub(CheckTerm::ofPlace(2), CheckTerm::ofConstant(1))
                .toString(),
            "($2 - 1)");
  EXPECT_EQ(CheckTerm::strnlen(CheckTerm::ofPlace(2), CheckTerm::ofConstant(16))
                .toString(),
            "strnlen($2, 16)");
}

TEST(CheckPlan, TermsAreWellFormedByKind) {
  EXPECT_TRUE(CheckTerm::ofConstant(1).isWellFormed());
  EXPECT_TRUE(CheckTerm::ofPlace(
                  1, {CheckPathStep::deref(), CheckPathStep::member("n")})
                  .isWellFormed());
  EXPECT_TRUE(CheckTerm::add(CheckTerm::ofPlace(1), CheckTerm::sizeOf(2))
                  .isWellFormed());
  CheckTerm oneSided{.kind = CheckTerm::Kind::Add};
  oneSided.operands.push_back(CheckTerm::ofConstant(1));
  EXPECT_FALSE(oneSided.isWellFormed());
  EXPECT_EQ(oneSided.toString(), "<malformed>");
  EXPECT_FALSE(
      CheckTerm::ofPlace(1, {CheckPathStep::member("")}).isWellFormed());
  CheckTerm constantWithPath = CheckTerm::ofConstant(1);
  constantWithPath.path.push_back(CheckPathStep::deref());
  EXPECT_FALSE(constantWithPath.isWellFormed());
  EXPECT_FALSE(
      CheckTerm::add(CheckTerm::ofConstant(1), oneSided).isWellFormed());
}

TEST(CheckPlan, Spellings) {
  EXPECT_EQ(toString(Template::Nonnull), "nonnull");
  EXPECT_EQ(toString(Template::Disjoint), "disjoint");
  EXPECT_EQ(toString(Form::IfNonZero), "if-non-zero");
  EXPECT_EQ(toString(Form::Violation), "violation");
  EXPECT_EQ(toString(Placement::ReplaceAccess), "replace-access");
  EXPECT_EQ(toString(Placement::BeforeCall), "before-call");
}

TEST(CheckPlan, OperandCountsFollowTheTemplates) {
  EXPECT_EQ(
      operandCount(Template::Nonnull, Form::Plain, Placement::WrapOperand), 0U);
  EXPECT_EQ(
      operandCount(Template::Nonnull, Form::IfNonZero, Placement::WrapArgument),
      1U);
  EXPECT_EQ(
      operandCount(Template::Nonnull, Form::Function, Placement::WrapOperand),
      0U);
  EXPECT_EQ(operandCount(Template::Index, Form::Plain, Placement::WrapIndex),
            1U);
  EXPECT_EQ(operandCount(Template::Span, Form::Plain, Placement::ReplaceAccess),
            3U);
  EXPECT_EQ(operandCount(Template::Len, Form::Plain, Placement::BeforeCall),
            2U);
  EXPECT_EQ(operandCount(Template::Len, Form::Plain, Placement::WrapArgument),
            1U);
  EXPECT_EQ(operandCount(Template::Len, Form::Result, Placement::ReplaceCall),
            1U);
  EXPECT_EQ(
      operandCount(Template::Disjoint, Form::Plain, Placement::WrapArgument),
      2U);
  EXPECT_EQ(operandCount(Template::Assert, Form::Plain, Placement::ReplaceCall),
            0U);
  EXPECT_EQ(
      operandCount(Template::Index, Form::Violation, Placement::BeforeCall),
      0U);
  // Combinations §10.2–§10.4 give no meaning.
  EXPECT_FALSE(
      operandCount(Template::Assert, Form::Plain, Placement::WrapOperand));
  EXPECT_FALSE(
      operandCount(Template::Len, Form::Result, Placement::BeforeCall));
  EXPECT_FALSE(
      operandCount(Template::Index, Form::Plain, Placement::WrapArgument));
  EXPECT_FALSE(
      operandCount(Template::Nonnull, Form::Function, Placement::WrapArgument));
  EXPECT_FALSE(
      operandCount(Template::Span, Form::Result, Placement::ReplaceAccess));
}

/// `char dst[16]; memcpy(dst, src, n);`: `len(n, 16)` before the call.
static CheckPlanEntry memcpyLen() {
  return CheckPlanEntry{
      .site = SiteId{.function = 0, .ordinal = 2},
      .facet = Facet::Spatial,
      .requirement = 0,
      .kind = Template::Len,
      .form = Form::Plain,
      .placement = Placement::BeforeCall,
      .operands = {CheckTerm::ofPlace(5), CheckTerm::ofConstant(16)}};
}

TEST(CheckPlan, EntriesAreWellFormed) {
  CheckPlanEntry entry = memcpyLen();
  EXPECT_TRUE(isWellFormed(entry));
  entry.operands.pop_back();
  EXPECT_FALSE(isWellFormed(entry));
  entry = memcpyLen();
  // §7.5: `c < e -> Counted(e + k)` is checked only when `e - c` is
  // non-zero.
  entry.guard = CheckTerm::sub(CheckTerm::ofPlace(1), CheckTerm::ofConstant(0));
  EXPECT_TRUE(isWellFormed(entry));
  entry.guard = CheckTerm{.kind = CheckTerm::Kind::Mul};
  EXPECT_FALSE(isWellFormed(entry));
}

TEST(CheckPlan, LedgerTemplatesAndHelpers) {
  CheckPlanEntry entry = memcpyLen();
  EXPECT_EQ(ledgerTemplate(entry), CheckTemplate::Len);
  EXPECT_EQ(helperName(entry), "__weavec_chk_len");
  entry.form = Form::Violation;
  EXPECT_EQ(ledgerTemplate(entry), CheckTemplate::Violation);
  EXPECT_EQ(helperName(entry), "");

  CheckPlanEntry nonnull{.kind = Template::Nonnull,
                         .form = Form::IfNonZero,
                         .placement = Placement::WrapArgument};
  EXPECT_EQ(helperName(nonnull), "__weavec_chk_nonnull_n");
  nonnull.form = Form::Function;
  EXPECT_EQ(helperName(nonnull), "__weavec_chk_nonnull_fn");
  CheckPlanEntry sprintfLen{.kind = Template::Len,
                            .form = Form::Result,
                            .placement = Placement::ReplaceCall};
  EXPECT_EQ(helperName(sprintfLen), "__weavec_chk_len_r");
  CheckPlanEntry verify{.kind = Template::Index,
                        .placement = Placement::WrapIndex,
                        .proven = true};
  EXPECT_EQ(helperName(verify), "__weavec_prv_index");
  EXPECT_EQ(facetCheck(verify),
            (FacetCheck{.kind = CheckTemplate::Index, .proven = true}));
}

TEST(CheckPlan, SortingIsCanonical) {
  CheckPlan plan;
  const SiteId first{.function = 0, .ordinal = 1};
  const SiteId second{.function = 1, .ordinal = 0};
  plan.add(CheckPlanEntry{
      .site = second, .facet = Facet::Null, .kind = Template::Nonnull});
  plan.add(CheckPlanEntry{.site = first,
                          .facet = Facet::Spatial,
                          .kind = Template::Index,
                          .placement = Placement::WrapIndex});
  plan.add(CheckPlanEntry{
      .site = first, .facet = Facet::Spatial, .kind = Template::Nonnull});
  plan.add(CheckPlanEntry{.site = first,
                          .facet = Facet::Spatial,
                          .requirement = 1,
                          .kind = Template::Len,
                          .placement = Placement::BeforeCall});
  plan.sort();
  ASSERT_EQ(plan.size(), 4U);
  EXPECT_EQ(plan.entries[0].site, first);
  // On one operand, nonnull is innermost, then index (§10.4).
  EXPECT_EQ(plan.entries[0].kind, Template::Nonnull);
  EXPECT_EQ(plan.entries[1].kind, Template::Index);
  EXPECT_EQ(plan.entries[2].requirement, 1);
  EXPECT_EQ(plan.entries[3].site, second);
  const auto forFirst = plan.entriesFor(first);
  ASSERT_EQ(forFirst.size(), 3U);
  EXPECT_EQ(forFirst[2]->kind, Template::Len);
  EXPECT_TRUE(plan.entriesFor(SiteId{.function = 9, .ordinal = 9}).empty());
  CheckPlan copy = plan;
  copy.sort();
  EXPECT_EQ(copy, plan);
}

} // namespace weavec::core
