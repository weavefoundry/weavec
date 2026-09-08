//===- SafetyTest.cpp - Checked contract algebra (RFC 0018) --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/CheckedIO.h"

#include <gtest/gtest.h>

namespace weavec::core {

static SafetyObligation obligation(SafetyOutcome outcome, unsigned line = 1) {
  return {
      .property = SafetyProperty::Bounds,
      .outcome = outcome,
      .location = {.file = "test.c", .line = line, .column = 2, .opaque = 0},
      .function = "f",
      .subject = "access",
      .reason = "bounds",
      .calls = {}};
}
static CheckedRequirement extent(unsigned index, std::int64_t bytes = 4) {
  return {.kind = CheckedRequirementKind::Extent,
          .path = SummaryPath::param(index),
          .other = {},
          .begin = PathAffine::ofConstant(0),
          .end = PathAffine::ofConstant(bytes),
          .family = {}};
}
TEST(SafetyLedger, WeakestOutcomeWinsOnEveryPermutation) {
  for (unsigned a = 0; a < 5; ++a)
    for (unsigned b = 0; b < 5; ++b) {
      SafetyLedger left;
      SafetyLedger right;
      left.add(obligation(static_cast<SafetyOutcome>(a)));
      right.add(obligation(static_cast<SafetyOutcome>(b)));
      auto reverse = right;
      reverse.join(left);
      left.join(right);
      EXPECT_EQ(left, reverse);
      EXPECT_EQ(left.entries().begin()->second.outcome,
                static_cast<SafetyOutcome>(std::max(a, b)));
      auto again = left;
      again.join(left);
      EXPECT_EQ(again, left);
      EXPECT_EQ(left.complete(), std::max(a, b) < 3);
    }
}
TEST(SafetyLedger, LimitsFailClosed) {
  SafetyLedger ledger;
  for (unsigned i = 0; i <= MaxSafetyObligations; ++i)
    ledger.add(obligation(SafetyOutcome::Proven, i));
  EXPECT_EQ(ledger.entries().size(), MaxSafetyObligations);
  EXPECT_TRUE(ledger.limited());
  EXPECT_FALSE(ledger.complete());
  auto entry = obligation(SafetyOutcome::Trusted);
  entry.calls.resize(MaxSafetyCallDepth + 1);
  SafetyLedger chain;
  chain.add(entry);
  EXPECT_TRUE(chain.limited());
  EXPECT_FALSE(chain.complete());
}
TEST(SafetyLedger, FrontendHandlesDoNotAffectIdentity) {
  auto entry = obligation(SafetyOutcome::Proven);
  const auto identity = entry.identity();
  entry.location.opaque = 17;
  EXPECT_EQ(entry.identity(), identity);
  SafetyLedger ledger;
  ledger.add(entry);
  EXPECT_EQ(ledger.entries().begin()->second.location.opaque, 0U);
}
TEST(SafetyState, DisabledDomainHasNoFactsAndCopiesDoNotShareMutation) {
  AnalysisState ordinary;
  EXPECT_FALSE(ordinary.safety);
  AnalysisState checked;
  checked.safety.emplace();
  checked.safety->initialized.insert(PlaceId{1});
  auto copied = checked;
  copied.safety->initialized.clear();
  EXPECT_TRUE(checked.safety->initialized.contains(PlaceId{1}));
  EXPECT_TRUE(ordinary.join(checked));
  ASSERT_TRUE(ordinary.safety);
  EXPECT_TRUE(ordinary.safety->initialized.empty());
  EXPECT_TRUE(checked.join(ordinary));
  EXPECT_TRUE(checked.safety->initialized.empty());
}
TEST(SafetyState, IntersectionRetainsOnlyInitializedBytes) {
  SafetyState a;
  SafetyState b;
  const PlaceId p{1};
  a.initialized.insert(p);
  a.pointers.insert(p);
  a.initialize(p,
               {.begin = Affine::ofConstant(0), .end = Affine::ofConstant(8)});
  b.initialize(p,
               {.begin = Affine::ofConstant(4), .end = Affine::ofConstant(12)});
  EXPECT_TRUE(a.join(b));
  EXPECT_TRUE(a.initialized.empty());
  EXPECT_TRUE(a.pointers.empty());
  ASSERT_EQ(a.memory.at(p).size(), 1U);
  EXPECT_EQ(a.memory.at(p).front(),
            (InitializedRange{Affine::ofConstant(4), Affine::ofConstant(8)}));
  EXPECT_FALSE(a.join(b));
  EXPECT_TRUE(a.join(SafetyState{}));
  EXPECT_TRUE(a.memory.empty());
}
TEST(SafetyState, AdjacentWritesMergeWithoutFillingGaps) {
  SafetyState state;
  const PlaceId p{1};
  state.initialize(
      p, {.begin = Affine::ofConstant(0), .end = Affine::ofConstant(2)});
  state.initialize(
      p, {.begin = Affine::ofConstant(4), .end = Affine::ofConstant(6)});
  EXPECT_EQ(state.memory.at(p).size(), 2U);
  state.initialize(
      p, {.begin = Affine::ofConstant(2), .end = Affine::ofConstant(4)});
  ASSERT_EQ(state.memory.at(p).size(), 1U);
  EXPECT_EQ(state.memory.at(p).front().end, Affine::ofConstant(6));
  state.copyMemory(p, PlaceId{2});
  state.forget(p);
  EXPECT_FALSE(state.memory.contains(p));
  EXPECT_TRUE(state.memory.contains(PlaceId{2}));
}
TEST(CheckedContract, PreconditionsUnionAndPostconditionsIntersect) {
  CheckedContract a;
  CheckedContract b;
  a.computed = b.computed = true;
  a.require(extent(0));
  b.require(extent(1));
  a.establish(extent(0));
  b.establish(extent(0));
  b.establish(extent(1));
  a.join(b);
  EXPECT_EQ(a.requirements.size(), 2U);
  EXPECT_EQ(a.establishes.size(), 1U);
  EXPECT_TRUE(a.complete());
  for (unsigned i = 0; i <= MaxSafetyRequirements; ++i)
    a.require(extent(i));
  EXPECT_TRUE(a.limited);
  EXPECT_FALSE(a.complete());
}
TEST(CheckedContract, PortableRoundTripAndCorruption) {
  CheckedContract contract;
  contract.computed = contract.selected = true;
  contract.signature = "void (char *, unsigned int)";
  contract.require(extent(0));
  auto writable = extent(0);
  writable.kind = CheckedRequirementKind::Writable;
  contract.require(writable);
  contract.establish(extent(1));
  contract.obligations.add(obligation(SafetyOutcome::Required));
  const GlobalNamer names = [](std::uint32_t) { return std::string("g"); };
  const GlobalResolver resolve = [](std::string_view) {
    return std::optional<std::uint32_t>(0);
  };
  const auto text = printCheckedContract(contract, names);
  EXPECT_EQ(parseCheckedContract(text, resolve), contract);
  for (std::size_t i = 0; i < text.size(); ++i)
    EXPECT_FALSE(parseCheckedContract(text.substr(0, i), resolve));
  EXPECT_FALSE(parseCheckedContract(text + "00", resolve));
  EXPECT_FALSE(parseCheckedContract("zz", resolve));
  FunctionSummary summary;
  summary.checked = contract;
  EXPECT_EQ(parseSummary(printSummary(summary, names), resolve), summary);
}
TEST(CheckedContract, MissingGlobalsInvalidateProof) {
  FunctionSummary summary;
  summary.checked.computed = true;
  auto requirement = extent(0);
  requirement.path = SummaryPath::global(0);
  summary.checked.require(requirement);
  const auto remapped = remapGlobals(
      summary, [](std::uint32_t) { return std::optional<std::uint32_t>{}; });
  EXPECT_TRUE(remapped.checked.limited);
  EXPECT_FALSE(remapped.checked.complete());
}
TEST(SafetyJson, EscapesControlsAndPreservesUnicode) {
  EXPECT_EQ(safetyJsonString("a\n\"\\"), "\"a\\u000a\\\"\\\\\"");
  EXPECT_EQ(safetyJsonString("é"), "\"é\"");
}
} // namespace weavec::core
