//===- FootprintTest.cpp - Conservation checks (RFC 0027) -----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Footprint.h"

#include <gtest/gtest.h>

#include <limits>

using namespace weavec::core;

static constexpr PlaceId Input{1};
static constexpr PlaceId Cursor{2};
static constexpr PlaceId Done{3};
static constexpr PlaceId Head{4};
static constexpr PlaceId Tail{5};

TEST(FootprintRelations, CopiesDoNotCreateEmptyOrIndependentAllocations) {
  FootprintRelations facts;
  facts.assign(Cursor, {{Input, 1}});
  EXPECT_TRUE(facts.equal(Cursor, Input));
  EXPECT_FALSE(facts.empty(Cursor));
  EXPECT_FALSE(facts.entails({{Cursor, 1}, {Input, 1}}));
  facts.assign(Cursor, {});
  EXPECT_TRUE(facts.empty(Cursor));
  EXPECT_FALSE(facts.empty(Input));
}

TEST(FootprintRelations, CleanupConservesAnInputAcrossRuntimeIterations) {
  FootprintRelations entry;
  entry.assign(Cursor, {{Input, 1}});
  entry.assign(Done, {});
  auto back = entry;
  // A structural proof supplies this split. Algebra alone cannot supply it.
  ASSERT_TRUE(back.constrain({{Cursor, 1}, {Head, -1}, {Tail, -1}}));
  back.assign(Done, {{Done, 1}, {Head, 1}});
  back.assign(Cursor, {{Tail, 1}});
  back.forget(Head);
  back.forget(Tail);
  entry.join(back);
  EXPECT_TRUE(entry.entails({{Cursor, 1}, {Done, 1}, {Input, -1}}));
  EXPECT_FALSE(entry.equal(Done, Input));
  entry.constrain({{Cursor, 1}});
  EXPECT_TRUE(entry.equal(Done, Input));
}

TEST(FootprintRelations, DroppingAHeadCannotProveCompleteConsumption) {
  FootprintRelations facts;
  facts.assign(Cursor, {{Input, 1}});
  facts.assign(Done, {});
  facts.constrain({{Cursor, 1}, {Head, -1}, {Tail, -1}});
  facts.assign(Cursor, {{Tail, 1}});
  facts.assign(Done, {{Done, 1}, {Cursor, 1}});
  EXPECT_FALSE(facts.equal(Done, Input));
  EXPECT_TRUE(facts.entails({{Done, 1}, {Head, 1}, {Input, -1}}));
}

TEST(FootprintRelations, JoinFindsCommonConservationInDifferentBases) {
  FootprintRelations left;
  FootprintRelations right;
  left.constrain({{Cursor, 1}, {Input, -1}});
  left.constrain({{Done, 1}});
  right.constrain({{Done, 1}, {Input, -1}});
  right.constrain({{Cursor, 1}});
  auto reverse = right;
  left.join(right);
  reverse.join(left);
  EXPECT_EQ(left, reverse);
  EXPECT_TRUE(left.entails({{Cursor, 1}, {Done, 1}, {Input, -1}}));
  EXPECT_FALSE(left.empty(Cursor));
  EXPECT_FALSE(left.empty(Done));
}

TEST(FootprintRelations, ForgettingOneAliasPreservesOtherAliases) {
  FootprintRelations facts;
  facts.assign(Cursor, {{Input, 1}});
  facts.assign(Done, {{Cursor, 1}});
  facts.forget(Cursor);
  EXPECT_TRUE(facts.equal(Done, Input));
  EXPECT_FALSE(facts.equal(Cursor, Input));
  facts.join(FootprintRelations{});
  EXPECT_TRUE(facts.all().empty());
}

TEST(FootprintRelations, DifferentAllocationIdentitiesCannotCancel) {
  FootprintRelations facts;
  facts.assign(Cursor, {{Head, 1}});
  facts.assign(Done, {{Tail, 1}});
  EXPECT_FALSE(facts.equal(Cursor, Done));
  facts.assign(Cursor, {{Cursor, 1}, {Tail, 1}});
  EXPECT_TRUE(facts.entails({{Cursor, 1}, {Head, -1}, {Tail, -1}}));
  EXPECT_FALSE(facts.entails({{Cursor, 1}, {Head, -2}}));
}

TEST(FootprintRelations, UnsupportedArithmeticAndCapacityLoseProof) {
  FootprintRelations facts;
  EXPECT_FALSE(
      facts.constrain({{Input, std::numeric_limits<std::int64_t>::min()}}));
  EXPECT_TRUE(facts.limited());
  EXPECT_FALSE(facts.empty(Input));
  FootprintRelations large;
  for (unsigned i = 0; i <= MaxFootprintVariables; ++i)
    large.constrain({{PlaceId{i + 1}, 1}});
  EXPECT_TRUE(large.limited());
  EXPECT_TRUE(large.all().empty());
}

TEST(FootprintRelations, AssignmentsRetainCoefficientsAndDetachCopies) {
  FootprintRelations facts;
  facts.assign(Cursor, {{Input, 2}});
  EXPECT_TRUE(facts.entails({{Cursor, 1}, {Input, -2}}));
  EXPECT_FALSE(facts.equal(Cursor, Input));
  auto copy = facts;
  copy.assign(Cursor, {{Cursor, -1}, {Input, 1}});
  EXPECT_TRUE(copy.entails({{Cursor, 1}, {Input, 1}}));
  EXPECT_TRUE(facts.entails({{Cursor, 1}, {Input, -2}}));
}

TEST(FootprintRelations, FiniteIdentityOracleChecksEveryPublishedEquality) {
  // Each bit denotes a different actual allocation. Enumerate partitions of
  // five objects into processed and remaining sets; no abstract operation
  // decides the concrete truth of an emitted equality.
  FootprintRelations invariant;
  invariant.assign(Cursor, {{Input, 1}});
  invariant.assign(Done, {});
  for (unsigned iteration = 0; iteration < 4; ++iteration) {
    auto next = invariant;
    next.constrain({{Cursor, 1}, {Head, -1}, {Tail, -1}});
    next.assign(Done, {{Done, 1}, {Head, 1}});
    next.assign(Cursor, {{Tail, 1}});
    next.forget(Head);
    next.forget(Tail);
    invariant.join(next);
  }
  for (unsigned mask = 0; mask < 32; ++mask) {
    const std::map<PlaceId, unsigned> concrete{
        {Input, 31}, {Cursor, mask}, {Done, 31U ^ mask}};
    for (const auto &row : invariant.all())
      for (unsigned allocation = 0; allocation < 5; ++allocation) {
        std::int64_t coefficient = 0;
        for (const auto &[place, factor] : row)
          if ((concrete.at(place) & (1U << allocation)) != 0)
            coefficient += factor;
        EXPECT_EQ(coefficient, 0) << "partition " << mask;
      }
  }
  EXPECT_TRUE(invariant.entails({{Cursor, 1}, {Done, 1}, {Input, -1}}));
}
