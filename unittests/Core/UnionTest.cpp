//===- UnionTest.cpp - Union view evidence (RFC 0025) --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Union.h"

#include "weavec/Core/CheckedIO.h"
#include "weavec/Core/Safety.h"

#include <gtest/gtest.h>

#include <array>

namespace weavec::core {

static UnionMember numberMember() {
  return {.object = {.bytes = 8, .alignment = 8, .identity = "union value"},
          .value = {.bytes = 4, .alignment = 4, .identity = "int"},
          .name = "number"};
}

TEST(UnionMember, CanonicalDescriptorIncludesTargetLayoutAndMemberIdentity) {
  const auto member = numberMember();
  const auto text = member.encode();
  EXPECT_EQ(UnionMember::decode(text), member);
  EXPECT_FALSE(UnionMember::decode(text + "x"));
  for (std::size_t end = 0; end < text.size(); ++end)
    EXPECT_FALSE(UnionMember::decode(text.substr(0, end)));
  for (const auto *invalid : {"", "union1:p:0:0:0:", "union2:p:",
                              "union1:s:99999:x", "union1:s:01:x"})
    EXPECT_FALSE(UnionMember::decode(invalid));
  auto changed = member;
  changed.name = "pointer";
  EXPECT_NE(changed.encode(), text);
  changed.name = "bad:name";
  EXPECT_FALSE(changed.valid());
  changed = member;
  changed.value.bytes = 16;
  EXPECT_FALSE(changed.valid());
}

TEST(UnionState, InvalidatedInputCannotBecomeANewEntryAssumption) {
  UnionState state;
  const PlaceId storage{1};
  EXPECT_TRUE(state.mayRequire(storage));
  state.invalidate(storage);
  EXPECT_FALSE(state.mayRequire(storage));
  state.set(storage, numberMember().encode());
  EXPECT_TRUE(state.members.contains(storage));
  EXPECT_FALSE(state.mayRequire(storage));
  state.invalidateAll();
  EXPECT_TRUE(state.members.empty());
  EXPECT_FALSE(state.mayRequire(PlaceId{2}));
}

TEST(UnionState, UnknownJoinDropsProofAndPreservesWriteHistory) {
  UnionState initialized;
  initialized.invalidate(PlaceId{1});
  initialized.set(PlaceId{1}, numberMember().encode());
  const auto original = initialized;
  EXPECT_FALSE(initialized.join(original, {{}}, {{}}));
  initialized.join({}, {{}}, {{}});
  EXPECT_TRUE(initialized.members.empty());
  EXPECT_FALSE(initialized.mayRequire(PlaceId{1}));
  UnionState unknown;
  unknown.join(original, {{}}, {{}});
  EXPECT_EQ(unknown, initialized);
}

TEST(UnionState, GuardedJoinRequiresDisjointIncomingPaths) {
  const PlaceId tag{1};
  const PlaceId object{2};
  PlaceGuard zero;
  zero.require(tag, ValueFact::ofConstant(0));
  PlaceGuard one;
  one.require(tag, ValueFact::ofConstant(1));
  UnionState first;
  first.set(object, numberMember().encode());
  auto alternate = numberMember();
  alternate.name = "alternate";
  UnionState second;
  second.set(object, alternate.encode());
  auto reverse = second;
  reverse.join(first, {one}, {zero});
  first.join(second, {zero}, {one});
  EXPECT_EQ(first, reverse);
  ASSERT_EQ(first.members.at(object).size(), 2U);
  EXPECT_FALSE(first.members.at(object).front().when.trivial());
  first.forgetDependency(tag);
  EXPECT_TRUE(first.members.empty());
  second.join({}, {one}, {one});
  EXPECT_TRUE(second.members.empty());
}

TEST(UnionState, CopyPreservesEvidenceIndependentlyOfSourceStorage) {
  const PlaceId source{1};
  const PlaceId destination{2};
  UnionState state;
  state.set(source, numberMember().encode());
  state.copy(source, destination);
  state.invalidate(source);
  EXPECT_TRUE(state.members.contains(destination));
  EXPECT_FALSE(state.mayRequire(destination));
  state.copy(source, destination);
  EXPECT_FALSE(state.members.contains(destination));
}

TEST(UnionState, GuardedMembersAgreeWithEveryConcreteTwoBranchExecution) {
  const PlaceId selector{1};
  const PlaceId storage{2};
  auto alternate = numberMember();
  alternate.name = "other";
  const std::vector<std::string> views{"", numberMember().encode(),
                                       alternate.encode()};
  std::array<PlaceGuard, 2> guards;
  guards[0].require(selector, ValueFact::ofConstant(0));
  guards[1].require(selector, ValueFact::ofConstant(1));
  // The concrete model has three states: uninitialized or either member.
  // Joining two executions must never turn the uninitialized state into proof.
  for (std::size_t left = 0; left < views.size(); ++left)
    for (std::size_t right = 0; right < views.size(); ++right) {
      UnionState a;
      UnionState b;
      if (left)
        a.set(storage, views[left]);
      if (right)
        b.set(storage, views[right]);
      a.join(b, {guards[0]}, {guards[1]});
      for (unsigned branch = 0; branch < 2; ++branch)
        for (std::size_t wanted = 1; wanted < views.size(); ++wanted) {
          bool proved = false;
          if (a.members.contains(storage))
            for (const auto &witness : a.members.at(storage)) {
              const auto condition = witness.when.conditions.find(selector);
              const bool applies =
                  condition == witness.when.conditions.end() ||
                  ValueFact::ofConstant(branch).implies(condition->second);
              proved |= witness.member == views[wanted] && applies;
            }
          EXPECT_EQ(proved, (branch == 0 ? left : right) == wanted);
        }
    }
}

TEST(UnionState, MemberEvidenceDoesNotInventPointerValidity) {
  const PlaceId storage{1};
  const PlaceId holder{2};
  UnionState state;
  state.set(storage, numberMember().encode());
  EXPECT_FALSE(state.members.at(storage).front().pointer);
  state.members.at(storage).front().pointer =
      UnionPointer{.holder = holder,
                   .storage = PlaceId{3},
                   .offset = {},
                   .extent = Affine::ofConstant(4),
                   .input = {},
                   .valid = true};
  state.forgetPointer(holder);
  EXPECT_FALSE(state.members.at(storage).front().pointer);
  EXPECT_EQ(state.members.at(storage).front().member, numberMember().encode());
}

TEST(UnionState, BudgetsFailClosedAndOrdinaryScalarForgetDoesNotSpendThem) {
  SafetyState state;
  for (std::size_t i = 0; i < MaxUnionObjects * 2; ++i)
    state.forget(PlaceId{static_cast<std::uint32_t>(i)});
  EXPECT_TRUE(state.unions.mayRequire(PlaceId{1}));
  for (std::size_t i = 0; i <= MaxUnionObjects; ++i)
    state.unions.invalidate(PlaceId{static_cast<std::uint32_t>(i)});
  EXPECT_FALSE(state.unions.mayRequire(PlaceId{999}));
}

TEST(UnionMember, CheckedTransportPreservesIndependentRequirementAndCases) {
  CheckedContract contract;
  contract.computed = true;
  contract.signature = "int (union value *)";
  contract.noteCaseInput(SummaryPath::param(1));
  contract.require({.kind = CheckedRequirementKind::UnionMember,
                    .path = SummaryPath::param(0).deref(),
                    .other = {},
                    .family = numberMember().encode()});
  contract.establish({.kind = CheckedRequirementKind::UnionMember,
                      .path = SummaryPath::result(),
                      .other = {},
                      .family = numberMember().encode()});
  EXPECT_EQ(parseCheckedContract(printCheckedContract(contract, {}), {}),
            contract);
  contract.require({.kind = CheckedRequirementKind::UnionMember,
                    .path = SummaryPath::param(0).deref(),
                    .other = {},
                    .family = "invalid"});
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
}

TEST(UnionMember, MalformedCaseCandidatesAndMemberPermissionsFailTransport) {
  CheckedContract contract;
  contract.computed = true;
  contract.caseInputs.insert(SummaryPath::result());
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
  contract.caseInputs.clear();
  for (std::uint32_t i = 0; i < 65; ++i)
    contract.caseInputs.insert(SummaryPath::param(i));
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
  contract.caseInputs.clear();
  auto deep = SummaryPath::param(0);
  for (std::size_t i = 0; i <= MaxHeapPathDepth; ++i)
    deep = deep.deref();
  contract.caseInputs.insert(deep);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
  contract.caseInputs.clear();
  CheckedRequirement member{.kind = CheckedRequirementKind::UnionMember,
                            .path = SummaryPath::param(0).deref(),
                            .other = {},
                            .family = numberMember().encode()};
  member.end = PathAffine::ofConstant(4);
  contract.requirements.insert(member);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
  contract.requirements.clear();
  member.end = PathAffine::ofConstant(0);
  member.ifNonNull = true;
  contract.requirements.insert(member);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
}

TEST(UnionMember, CandidateBudgetIsDeterministicAndNeverInventsProof) {
  CheckedContract forward;
  CheckedContract reverse;
  forward.computed = reverse.computed = true;
  for (std::uint32_t i = 0; i < 100; ++i) {
    forward.noteCaseInput(SummaryPath::param(i));
    reverse.noteCaseInput(SummaryPath::param(99 - i));
  }
  EXPECT_EQ(forward, reverse);
  EXPECT_EQ(forward.caseInputs.size(), 64U);
  EXPECT_TRUE(forward.requirements.empty());
  EXPECT_TRUE(forward.establishes.empty());
  EXPECT_FALSE(forward.limited);
}

} // namespace weavec::core
