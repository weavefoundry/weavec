//===- AliasRelationTest.cpp - Tests for the may-alias relation -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AliasRelation.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

using Members = std::vector<PlaceId>;

constexpr PlaceId P{0};
constexpr PlaceId Q{1};
constexpr PlaceId R{2};
constexpr PlaceId S{3};

TEST(AliasRelation, EveryPlaceAliasesItself) {
  const AliasRelation aliases;
  EXPECT_TRUE(aliases.mayAlias(P, P));
  EXPECT_FALSE(aliases.mayAlias(P, Q));
  EXPECT_EQ(aliases.members(P), Members{P});
  EXPECT_EQ(aliases.size(), 0U);
}

TEST(AliasRelation, UniteIsSymmetric) {
  AliasRelation aliases;
  aliases.unite(Q, P);
  EXPECT_TRUE(aliases.mayAlias(P, Q));
  EXPECT_TRUE(aliases.mayAlias(Q, P));
  EXPECT_EQ(aliases.members(P), (Members{P, Q}));
  EXPECT_EQ(aliases.members(Q), (Members{P, Q}));
}

TEST(AliasRelation, CopyChainIsClosed) {
  // q = p; r = q;  -- r must alias p too, or `free(r); use(p)` is missed.
  AliasRelation aliases;
  aliases.unite(Q, P);
  aliases.unite(R, Q);
  EXPECT_TRUE(aliases.mayAlias(R, P));
  EXPECT_EQ(aliases.members(P), (Members{P, Q, R}));
}

TEST(AliasRelation, SeparateDropsOnlyThatPlace) {
  AliasRelation aliases;
  aliases.unite(Q, P);
  aliases.unite(R, Q);
  aliases.separate(Q);
  EXPECT_FALSE(aliases.mayAlias(Q, P));
  EXPECT_FALSE(aliases.mayAlias(Q, R));
  EXPECT_TRUE(aliases.mayAlias(P, R)) << "p and r still hold the same value";
  EXPECT_EQ(aliases.members(Q), Members{Q});
}

TEST(AliasRelation, SeparateLastPairLeavesRelationEmpty) {
  AliasRelation aliases;
  aliases.unite(Q, P);
  aliases.separate(P);
  EXPECT_EQ(aliases.size(), 0U);
  EXPECT_EQ(aliases, AliasRelation{}) << "canonical: no dangling entries";
}

TEST(AliasRelation, JoinIsUnionOfPairs) {
  AliasRelation left;
  left.unite(Q, P);
  AliasRelation right;
  right.unite(R, P);

  left.join(right);
  EXPECT_TRUE(left.mayAlias(P, Q));
  EXPECT_TRUE(left.mayAlias(P, R));
  // Not transitively closed: q and r were never copied from each other on
  // any single path.
  EXPECT_FALSE(left.mayAlias(Q, R));
}

TEST(AliasRelation, JoinIsIdempotentAndCommutative) {
  AliasRelation a;
  a.unite(Q, P);
  AliasRelation b;
  b.unite(S, R);

  AliasRelation ab = a;
  ab.join(b);
  AliasRelation ba = b;
  ba.join(a);
  EXPECT_EQ(ab, ba);

  AliasRelation again = ab;
  again.join(ab);
  EXPECT_EQ(again, ab);
}

TEST(AliasRelation, PairsAreOrdered) {
  AliasRelation aliases;
  aliases.unite(R, P);
  aliases.unite(Q, P);
  using Pair = std::pair<PlaceId, PlaceId>;
  EXPECT_EQ(aliases.pairs(), (std::vector<Pair>{{P, Q}, {P, R}, {Q, R}}));
}

} // namespace
} // namespace weavec::core
