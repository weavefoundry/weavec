//===- RelationTest.cpp - Tests for RelationTracker -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Relation.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

constexpr PlaceId I{0};
constexpr PlaceId N{1};
constexpr PlaceId K{2};

// RFC 0011, *Relations*.
TEST(Relation, NarrowsAndWidens) {
  EXPECT_EQ(narrow(Relation::LessEqual, Relation::Less), Relation::Less);
  EXPECT_EQ(narrow(Relation::LessEqual, Relation::GreaterEqual),
            Relation::Equal);
  EXPECT_FALSE(narrow(Relation::Less, Relation::Greater));
  EXPECT_FALSE(narrow(Relation::Less, Relation::Equal));
  EXPECT_EQ(widen(Relation::Less, Relation::Equal), Relation::LessEqual);
  EXPECT_EQ(widen(Relation::Less, Relation::Less), Relation::Less);
  EXPECT_FALSE(widen(Relation::Less, Relation::Greater))
      << "not-equal is not kept";
  EXPECT_FALSE(widen(Relation::LessEqual, Relation::Greater)) << "anything";
  EXPECT_EQ(flipped(Relation::Less), Relation::Greater);
  EXPECT_EQ(flipped(Relation::LessEqual), Relation::GreaterEqual);
  EXPECT_EQ(flipped(Relation::Equal), Relation::Equal);
  EXPECT_EQ(spelling(Relation::GreaterEqual), ">=");
}

TEST(RelationTracker, LearnsInEitherOrderAndNarrows) {
  RelationTracker relations;
  relations.learn(I, Relation::Less, N); // i < n
  EXPECT_EQ(relations.between(I, N), Relation::Less);
  EXPECT_EQ(relations.between(N, I), Relation::Greater);
  EXPECT_EQ(relations.between(I, I), Relation::Equal);
  EXPECT_FALSE(relations.between(I, K));

  relations.learn(N, Relation::GreaterEqual, I); // n >= i: no news
  EXPECT_EQ(relations.between(I, N), Relation::Less);

  // i <= n then i == n: equal. A contradiction is replaced, not kept.
  RelationTracker loop;
  loop.learn(I, Relation::LessEqual, N);
  loop.learn(I, Relation::Equal, N);
  EXPECT_EQ(loop.between(I, N), Relation::Equal);
  loop.learn(I, Relation::Greater, N);
  EXPECT_EQ(loop.between(I, N), Relation::Greater);

  // Writing either side forgets the pair.
  loop.forget(N);
  EXPECT_FALSE(loop.between(I, N));
  EXPECT_TRUE(loop.empty());
}

TEST(RelationTracker, JoinKeepsTheWeakestRelationBothSidesImply) {
  RelationTracker a;
  a.learn(I, Relation::Less, N);
  a.learn(I, Relation::Less, K);
  RelationTracker b;
  b.learn(I, Relation::Equal, N);
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.between(I, N), Relation::LessEqual);
  EXPECT_FALSE(a.between(I, K)) << "only on one side";
  EXPECT_FALSE(a.join(b)) << "fixpoint";

  RelationTracker c;
  c.learn(I, Relation::Less, N);
  RelationTracker d;
  d.learn(I, Relation::Greater, N);
  EXPECT_TRUE(c.join(d));
  EXPECT_FALSE(c.between(I, N)) << "`<` or `>` is nothing this tracker keeps";
}

// `j = i; if (j < n)`: a relation reaches through one equality, either side.
TEST(RelationTracker, LooksThroughOneEquality) {
  RelationTracker t;
  t.learn(I, Relation::Equal, K);
  t.learn(K, Relation::Less, N);
  EXPECT_EQ(t.between(I, N), Relation::Less);
  EXPECT_EQ(t.between(N, I), Relation::Greater);
  RelationTracker u;
  u.learn(N, Relation::Equal, K);
  u.learn(I, Relation::GreaterEqual, K);
  EXPECT_EQ(u.between(I, N), Relation::GreaterEqual);
  t.forget(K);
  EXPECT_FALSE(t.between(I, N)) << "the hop is gone with `k`";
}

// RFC 0011, *Extents in summaries*: a place compared with a constant by an
// ordering conditions the path in a way no guard spells; a write clears it.
TEST(RelationTracker, BoundsConditionAPlaceUntilItIsWritten) {
  RelationTracker t;
  EXPECT_FALSE(t.conditions(N));
  t.noteBounded(N);
  EXPECT_TRUE(t.isBounded(N));
  EXPECT_TRUE(t.conditions(N));
  EXPECT_FALSE(t.empty());
  t.learn(I, Relation::Less, K);
  EXPECT_TRUE(t.conditions(I)) << "related to another place";
  EXPECT_TRUE(t.conditions(K));
  t.forget(N);
  EXPECT_FALSE(t.isBounded(N));
  EXPECT_FALSE(t.conditions(N));

  // A bound on either side survives the join: the path is conditioned if
  // any way in was.
  RelationTracker a;
  RelationTracker b;
  b.noteBounded(N);
  EXPECT_TRUE(a.join(b));
  EXPECT_TRUE(a.isBounded(N));
  EXPECT_FALSE(a.join(b)) << "fixpoint";
}

// RFC 0011, *Relations*: `i < 8` bounds `i` above by a constant that a
// later, tighter comparison narrows, a write clears, and a join widens.
TEST(RelationTracker, UpperBoundsNarrowForgetAndJoin) {
  RelationTracker t;
  EXPECT_FALSE(t.atMost(I));
  t.learnAtMost(I, 7);
  EXPECT_EQ(t.atMost(I), 7);
  EXPECT_TRUE(t.isBounded(I)) << "a bound is a condition on the path";
  EXPECT_FALSE(t.empty());
  t.learnAtMost(I, 10);
  EXPECT_EQ(t.atMost(I), 7) << "the tighter bound stays";
  t.learnAtMost(I, 3);
  EXPECT_EQ(t.atMost(I), 3) << "narrowed";
  t.forget(I);
  EXPECT_FALSE(t.atMost(I));
  EXPECT_TRUE(t.empty());

  // `j = i; if (j < 8)`: `i`'s bound is `j`'s, one hop through the
  // equality.
  t.learn(I, Relation::Equal, K);
  t.learnAtMost(K, 7);
  EXPECT_EQ(t.atMost(I), 7);
  EXPECT_FALSE(t.atMost(N));

  // The join keeps a bound both sides know, as the larger; a bound one side
  // lacks is dropped.
  RelationTracker a;
  RelationTracker b;
  a.learnAtMost(I, 7);
  a.learnAtMost(N, 4);
  b.learnAtMost(I, 9);
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.atMost(I), 9);
  EXPECT_FALSE(a.atMost(N));
  EXPECT_TRUE(a.isBounded(N)) << "still conditioned";
  EXPECT_FALSE(a.join(b)) << "fixpoint";
}

} // namespace
} // namespace weavec::core
