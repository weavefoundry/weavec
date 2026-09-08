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
  EXPECT_FALSE(chain.limited());
  EXPECT_TRUE(chain.complete());
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
// RFC 0019: conditional must-facts may survive a merge only when the other
// edge excludes their premise. A subsequent assignment destroys the evidence.
TEST(SafetyState, ConditionalInitializationExcludesTheOtherEdge) {
  const PlaceId bytes{1};
  const PlaceId flag{2};
  PlaceGuard yes;
  PlaceGuard no;
  yes.require(flag, ValueFact::of(Outcome::Positive));
  no.require(flag, ValueFact::of(Outcome::Zero));
  SafetyState written;
  written.initialize(bytes, {.begin = {}, .end = Affine::ofConstant(4)});
  const auto before = written;
  SafetyState empty;
  EXPECT_TRUE(written.join(empty, yes, no));
  ASSERT_EQ(written.memory.at(bytes).size(), 1U);
  EXPECT_EQ(written.memory.at(bytes).front().when, yes);
  auto reverse = empty;
  reverse.join(before, no, yes);
  EXPECT_EQ(written, reverse);
  EXPECT_FALSE(written.join(empty, yes, no));
  written.forgetDependency(flag);
  EXPECT_TRUE(written.memory.at(bytes).empty());
}

TEST(SafetyState, MissingBranchEvidenceCannotCreateConditionalFacts) {
  const PlaceId bytes{1};
  const PlaceId flag{2};
  PlaceGuard yes;
  yes.require(flag, ValueFact::of(Outcome::Positive));
  SafetyState a;
  a.initialize(bytes, {.begin = {}, .end = Affine::ofConstant(4)});
  a.join(SafetyState{}, yes, {});
  EXPECT_TRUE(a.memory.empty());
}

TEST(SafetyState, GuardLimitCannotWeakenMustFactPremises) {
  const PlaceId bytes{1};
  PlaceGuard branch;
  for (unsigned i = 0; i < MaxGuardConjuncts; ++i)
    branch.require(PlaceId{10 + i}, ValueFact::of(Outcome::Positive));
  PlaceGuard original;
  original.require(PlaceId{30}, ValueFact::of(Outcome::Positive));
  PlaceGuard other;
  other.require(PlaceId{10}, ValueFact::of(Outcome::Zero));
  SafetyState a;
  a.initialize(bytes,
               {.begin = {}, .end = Affine::ofConstant(4), .when = original});
  a.join(SafetyState{}, branch, other);
  EXPECT_TRUE(a.memory.empty());
}

TEST(SafetyState, ReplacingHolderDoesNotReplaceItsFormerObject) {
  SafetyState state;
  const PlaceId holder{1};
  const PlaceId copy{2};
  const PlaceId object{3};
  state.objects[holder] = object;
  state.objects[copy] = object;
  state.initialize(object, {.begin = {}, .end = Affine::ofConstant(4)});
  state.forget(holder);
  EXPECT_FALSE(state.objects.contains(holder));
  EXPECT_EQ(state.objects.at(copy), object);
  EXPECT_TRUE(state.memory.contains(object));
  auto unknown = state;
  unknown.objects.erase(copy);
  state.join(unknown);
  EXPECT_FALSE(state.objects.contains(copy));
}

TEST(PendingOutcome, InitializationRequiresEverySelectedReturnClass) {
  const PlaceId object{1};
  const InitializedRange bytes{.begin = {}, .end = Affine::ofConstant(4)};
  PendingOutcome call;
  call.consumedBy.try_emplace(Outcome::Zero);
  call.consumedBy.try_emplace(Outcome::Positive);
  call.initializedOn[Outcome::Positive].emplace_back(object, bytes);
  EXPECT_TRUE(call.initializedInAll().empty());
  auto success = call;
  success.select({Outcome::Positive});
  ASSERT_EQ(success.initializedInAll().size(), 1U);
  auto failure = call;
  failure.select({Outcome::Zero});
  EXPECT_TRUE(failure.initializedInAll().empty());
  EXPECT_TRUE(success.unite(failure));
  EXPECT_TRUE(success.initializedInAll().empty());
  auto changed = call;
  changed.initializedOn.clear();
  EXPECT_FALSE(call.unite(changed));
}

TEST(PendingOutcome, ReassignmentDropsPendingInitializationDependencies) {
  AnalysisState state;
  state.safety.emplace();
  const PlaceId result{1};
  const PlaceId object{2};
  const PlaceId length{3};
  auto &call = state.pending[result];
  call.consumedBy.try_emplace(Outcome::Positive);
  call.initializedOn[Outcome::Positive].push_back(
      {object, {.begin = {}, .end = Affine::ofPlace(length)}});
  state.dropGuardsOn(length);
  EXPECT_TRUE(call.initializedInAll().empty());
}

TEST(CheckedIO, ConditionalNestedAndResultInitializationRoundTrip) {
  CheckedContract contract;
  contract.computed = true;
  contract.signature = "ptr(ptr,i32)";
  auto post = extent(0);
  post.kind = CheckedRequirementKind::Initialized;
  post.path = SummaryPath::result();
  post.on = Outcome::NonNull;
  post.when.require(SummaryPath::param(1), ValueFact::of(Outcome::Positive));
  contract.establish(post);
  post.kind = CheckedRequirementKind::Terminated;
  post.path = SummaryPath::param(0);
  post.on.reset();
  contract.require(post);
  const GlobalNamer names = [](std::uint32_t) { return std::string("global"); };
  const GlobalResolver resolve = [](std::string_view) {
    return std::optional<std::uint32_t>{1};
  };
  const auto encoded = printCheckedContract(contract, names);
  ASSERT_FALSE(encoded.empty());
  const auto decoded = parseCheckedContract(encoded, resolve);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, contract);
}

TEST(PendingOutcome, OverwritingOutputDiscardsNumericPostcondition) {
  AnalysisState state;
  const PlaceId result{1};
  const PlaceId output{2};
  auto &call = state.pending[result];
  call.consumedBy.try_emplace(Outcome::Zero);
  call.factOn[Outcome::Zero].emplace_back(output, ValueFact::ofConstant(16));
  ASSERT_EQ(call.factsInAll().size(), 1U);
  state.dropGuardsOn(output);
  EXPECT_TRUE(call.factsInAll().empty());
}

TEST(SafetyState, CopiedIncomingEvidenceIsNotAnInitializedRange) {
  const PlaceId object{1};
  const PlaceId source{2};
  SafetyState copied;
  copied.initialize(
      object, {.begin = {}, .end = Affine::ofConstant(16), .source = source});
  copied.initialize(object, {.begin = {}, .end = Affine::ofConstant(4)});
  ASSERT_EQ(copied.memory.at(object).size(), 2U);
  auto empty = copied;
  empty.memory.clear();
  copied.join(empty);
  EXPECT_TRUE(copied.memory.empty());
}

TEST(CheckedIO, RejectsOutputFactsInAnInputContract) {
  const auto names = [](std::uint32_t) { return std::string("global"); };
  const auto resolve = [](std::string_view) { return std::optional(1U); };
  CheckedContract contract;
  contract.computed = true;
  auto requirement = extent(0);
  requirement.kind = CheckedRequirementKind::Copied;
  contract.require(requirement);
  EXPECT_FALSE(
      parseCheckedContract(printCheckedContract(contract, names), resolve));
  contract.requirements.clear();
  requirement.kind = CheckedRequirementKind::Initialized;
  requirement.on = Outcome::Zero;
  contract.require(requirement);
  EXPECT_FALSE(
      parseCheckedContract(printCheckedContract(contract, names), resolve));
}

TEST(CheckedIO, MissingOutputGuardGlobalsInvalidateProof) {
  FunctionSummary summary;
  summary.checked.computed = true;
  auto post = extent(0);
  post.kind = CheckedRequirementKind::Initialized;
  post.path = SummaryPath::result().field("bytes");
  post.on = Outcome::NonNull;
  post.when.require(SummaryPath::global(1), ValueFact::of(Outcome::Positive));
  summary.checked.establish(post);
  const auto names = [](std::uint32_t) { return std::string("flag"); };
  const auto missing = [](std::string_view) {
    return std::optional<std::uint32_t>{};
  };
  EXPECT_FALSE(parseCheckedContract(
      printCheckedContract(summary.checked, names), missing));
  const auto remapped = remapGlobals(
      summary, [](std::uint32_t) { return std::optional<std::uint32_t>{}; });
  EXPECT_TRUE(remapped.checked.limited);
  EXPECT_FALSE(remapped.checked.complete());
  EXPECT_TRUE(remapped.checked.establishes.empty());
}

TEST(CheckedIO, BytePreservationAndZeroesCannotBecomeInputAssumptions) {
  const auto names = [](std::uint32_t) { return std::string("global"); };
  const auto resolve = [](std::string_view) { return std::optional(1U); };
  for (const auto kind :
       {CheckedRequirementKind::Copied, CheckedRequirementKind::Zeroed}) {
    CheckedContract contract;
    contract.computed = true;
    auto post = extent(0);
    post.kind = kind;
    post.path = SummaryPath::result();
    post.on = Outcome::NonNull;
    contract.establish(post);
    const auto encoded = printCheckedContract(contract, names);
    EXPECT_EQ(parseCheckedContract(encoded, resolve), contract);
    for (std::size_t i = 0; i < encoded.size(); ++i)
      EXPECT_FALSE(parseCheckedContract(encoded.substr(0, i), resolve));
    contract.establishes.clear();
    post.path = SummaryPath::param(0);
    post.on.reset();
    contract.require(post);
    EXPECT_FALSE(
        parseCheckedContract(printCheckedContract(contract, names), resolve));
  }
}

TEST(SafetyState, ZeroBytesImplyInitializationAndAreInvalidatedByWrites) {
  const PlaceId bytes{1};
  SafetyState state;
  state.initialize(bytes,
                   {.begin = {}, .end = Affine::ofConstant(4), .zeroed = true});
  ASSERT_EQ(state.memory.at(bytes).size(), 2U);
  const auto initialized = std::ranges::find_if(
      state.memory.at(bytes), [](const auto &range) { return !range.zeroed; });
  ASSERT_NE(initialized, state.memory.at(bytes).end());
  auto other = state;
  other.forgetZeros();
  ASSERT_EQ(other.memory.at(bytes).size(), 1U);
  state.join(other);
  EXPECT_EQ(state, other);
}

TEST(PendingOutcome, AWriteCannotReapplyAnEarlierTerminatorFact) {
  AnalysisState state;
  state.safety.emplace();
  const PlaceId result{1};
  auto &call = state.pending[result];
  call.consumedBy.try_emplace(Outcome::Zero);
  call.initializedOn[Outcome::Zero].push_back(
      {PlaceId{2},
       {.begin = {}, .end = Affine::ofConstant(1), .zeroed = true}});
  state.forgetZeroedMemory();
  EXPECT_TRUE(call.initializedInAll().empty());
}

TEST(SafetyState, HeapCellSnapshotsKeepTheReferentIdentity) {
  SafetyState state;
  const PlaceId holder{1};
  const PlaceId saved{2};
  const PlaceId object{3};
  state.objects[holder] = object;
  state.initialize(object, {.begin = {}, .end = Affine::ofConstant(4)});
  state.copyMemory(holder, saved);
  state.forget(holder);
  ASSERT_TRUE(state.objects.contains(saved));
  EXPECT_EQ(state.objects.at(saved), object);
  ASSERT_TRUE(state.memory.contains(object));
  EXPECT_EQ(state.memory.at(object).front().end, Affine::ofConstant(4));
}

} // namespace weavec::core
