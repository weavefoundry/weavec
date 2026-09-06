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

// RFC 0012, *Offset relations*: `i < n - 1` is an edge with an offset; it
// is not a relation `between` reports, but `edgeBetween` does, from either
// side, and narrowing and joining act on the difference the edge bounds.
TEST(RelationTracker, OffsetRelationsAreEdgesNotRelations) {
  RelationTracker t;
  t.learn(I, Relation::Less, N, -1); // i < n - 1, kept as i <= n - 2
  EXPECT_FALSE(t.between(I, N)) << "an offset is not a plain relation";
  const auto edge = t.edgeBetween(I, N);
  ASSERT_TRUE(edge);
  EXPECT_EQ(*edge,
            (RelationEdge{.relation = Relation::LessEqual, .offset = -2}));
  EXPECT_EQ(t.edgeBetween(N, I),
            (RelationEdge{.relation = Relation::GreaterEqual, .offset = 2}));
  EXPECT_TRUE(t.conditions(I));

  // `i <= n - 1` says the same as `i < n`: normalised to the plain form.
  RelationTracker u;
  u.learn(I, Relation::LessEqual, N, -1);
  EXPECT_EQ(u.between(I, N), Relation::Less);
  u.learn(I, Relation::Greater, N, -1); // i > n - 1, i.e. i >= n: contradiction
  EXPECT_EQ(u.between(I, N), Relation::GreaterEqual) << "the second fact wins";

  // Narrowing: `i < n` then `i < n - 1` keeps the tighter; the reverse too.
  RelationTracker v;
  v.learn(I, Relation::Less, N);
  v.learn(I, Relation::Less, N, -1);
  EXPECT_EQ(v.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::LessEqual, .offset = -2}));
  v.learn(I, Relation::Less, N);
  EXPECT_EQ(v.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::LessEqual, .offset = -2}));
  // `i <= n - 3` and `i >= n - 3` meet at `i == n - 3`.
  RelationTracker w;
  w.learn(I, Relation::LessEqual, N, -3);
  w.learn(I, Relation::GreaterEqual, N, -3);
  EXPECT_EQ(w.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::Equal, .offset = -3}));
  // `i < n + 3` and `i > n - 3` bound the difference on both sides: no
  // one edge spells that; the second wins (normalised: `i >= n - 2`).
  RelationTracker x;
  x.learn(I, Relation::Less, N, 3);
  x.learn(I, Relation::Greater, N, -3);
  EXPECT_EQ(x.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::GreaterEqual, .offset = -2}));
}

// `j = i + 1; if (j < n)`: `i < n - 1`, the equality's offset composed with
// the relation's.
TEST(RelationTracker, ComposesOffsetsThroughTheEqualityHop) {
  RelationTracker t;
  t.learn(K, Relation::Equal, I, 1); // k == i + 1
  t.learn(K, Relation::Less, N);     // k < n
  EXPECT_EQ(t.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::LessEqual, .offset = -2}));
  EXPECT_EQ(t.edgeBetween(N, I),
            (RelationEdge{.relation = Relation::GreaterEqual, .offset = 2}));
  EXPECT_FALSE(t.between(I, N));
  // `k == i + 1; k <= n - 1` is `i <= n - 2`.
  RelationTracker u;
  u.learn(K, Relation::Equal, I, 1);
  u.learn(K, Relation::LessEqual, N, -1);
  EXPECT_EQ(u.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::LessEqual, .offset = -2}));
  // Through the other side: `n == k + 2; i < k` is `i < n - 2`.
  RelationTracker v;
  v.learn(N, Relation::Equal, K, 2);
  v.learn(I, Relation::Less, K);
  EXPECT_EQ(v.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::LessEqual, .offset = -3}));
  // The equality's offset reaches the constant bounds too: `k == i + 1; k
  // <= 7` is `i <= 6`.
  EXPECT_EQ(t.atMost(I), std::nullopt);
  t.learnAtMost(K, 7);
  EXPECT_EQ(t.atMost(I), 6);
  t.learnAtLeast(K, 2);
  EXPECT_EQ(t.atLeast(I), 1);
}

// The join takes the hull of the differences: `i < n - 1` or `i < n` is `i
// < n`; `i == n - 1` or `i == n + 1` is nothing one edge spells.
TEST(RelationTracker, JoinsOffsetEdgesByTheirHull) {
  RelationTracker a;
  a.learn(I, Relation::Less, N, -1);
  RelationTracker b;
  b.learn(I, Relation::Less, N);
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.between(I, N), Relation::Less);
  EXPECT_FALSE(a.join(b)) << "fixpoint";

  RelationTracker c;
  c.learn(I, Relation::Equal, N, -1);
  RelationTracker d;
  d.learn(I, Relation::Equal, N, 1);
  EXPECT_TRUE(c.join(d));
  EXPECT_FALSE(c.edgeBetween(I, N));

  // `i == n - 1` or `i < n - 1`: `i < n`.
  RelationTracker e;
  e.learn(I, Relation::Equal, N, -1);
  RelationTracker f;
  f.learn(I, Relation::Less, N, -1);
  EXPECT_TRUE(e.join(f));
  EXPECT_EQ(e.between(I, N), Relation::Less);

  // `i <= n - 2` or `i <= n - 3`: the looser, `i <= n - 2`.
  RelationTracker g;
  g.learn(I, Relation::LessEqual, N, -3);
  RelationTracker h;
  h.learn(I, Relation::LessEqual, N, -2);
  EXPECT_TRUE(g.join(h));
  EXPECT_EQ(g.edgeBetween(I, N),
            (RelationEdge{.relation = Relation::LessEqual, .offset = -2}));
}

// RFC 0012, *Lower bounds*: `i >= 8` is kept beside the upper bounds, the
// larger bound winning on narrowing, the smaller on the join, a write
// clearing it.
TEST(RelationTracker, LowerBoundsNarrowForgetAndJoin) {
  RelationTracker t;
  EXPECT_FALSE(t.atLeast(I));
  t.learnAtLeast(I, 8);
  EXPECT_EQ(t.atLeast(I), 8);
  EXPECT_TRUE(t.isBounded(I));
  EXPECT_TRUE(t.conditions(I));
  EXPECT_FALSE(t.empty());
  t.learnAtLeast(I, 5);
  EXPECT_EQ(t.atLeast(I), 8) << "the tighter bound stays";
  t.learnAtLeast(I, 10);
  EXPECT_EQ(t.atLeast(I), 10) << "narrowed";
  EXPECT_FALSE(t.atMost(I)) << "independent of the upper bound";
  t.learnAtMost(I, 12);
  EXPECT_EQ(t.atLeast(I), 10);
  EXPECT_EQ(t.atMost(I), 12);
  t.forget(I);
  EXPECT_FALSE(t.atLeast(I));
  EXPECT_TRUE(t.empty());

  // One hop through an equality.
  t.learn(I, Relation::Equal, K);
  t.learnAtLeast(K, 3);
  EXPECT_EQ(t.atLeast(I), 3);

  RelationTracker a;
  RelationTracker b;
  a.learnAtLeast(I, 8);
  a.learnAtLeast(N, 4);
  b.learnAtLeast(I, 6);
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.atLeast(I), 6) << "the smaller bound";
  EXPECT_FALSE(a.atLeast(N)) << "only on one side";
  EXPECT_TRUE(a.isBounded(N)) << "still conditioned";
  EXPECT_FALSE(a.join(b)) << "fixpoint";
}

} // namespace
} // namespace weavec::core
