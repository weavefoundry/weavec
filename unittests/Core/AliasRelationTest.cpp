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

  EXPECT_TRUE(left.join(right));
  EXPECT_TRUE(left.mayAlias(P, Q));
  EXPECT_TRUE(left.mayAlias(P, R));
  // Not transitively closed: q and r were never copied from each other on
  // any single path.
  EXPECT_FALSE(left.mayAlias(Q, R));
  EXPECT_FALSE(left.join(right)) << "nothing new: unchanged";
  EXPECT_FALSE(left.join(AliasRelation{}));
}

TEST(AliasRelation, JoinReportsAnEdgeLosingExactness) {
  AliasRelation exact;
  exact.unite(Q, P);
  AliasRelation interior;
  interior.unite(Q, P, PointerOffset::unknown());
  EXPECT_TRUE(exact.join(interior)) << "the edge changed: it is interior now";
  EXPECT_FALSE(exact.isExact(P, Q));
  EXPECT_FALSE(exact.join(interior));
}

// RFC 0006, *Loans end at the last use of their holder*: a dead local's
// edges go; the places it was related to keep their own.
TEST(AliasRelation, SeparateIfDropsEveryMatchingPlace) {
  AliasRelation aliases;
  aliases.unite(Q, P);
  aliases.unite(R, P);
  aliases.unite(S, R);
  aliases.separateIf([](PlaceId place) { return place == Q || place == S; });
  EXPECT_EQ(aliases.members(Q), Members{Q});
  EXPECT_EQ(aliases.members(S), Members{S});
  EXPECT_TRUE(aliases.mayAlias(P, R)) << "p and r were copied on one path";
  EXPECT_EQ(aliases.members(P), (Members{P, R}));
  aliases.separateIf([](PlaceId) { return true; });
  EXPECT_EQ(aliases, AliasRelation{});
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

// RFC 0006, *Alias exactness*.
TEST(AliasRelation, ExactnessPropagatesAndRefutesOnlyExactEdges) {
  AliasRelation aliases;
  aliases.unite(Q, P);                               // q = p
  aliases.unite(R, P, PointerOffset::ofElements(1)); // r = p + 1
  EXPECT_TRUE(aliases.isExact(P, Q));
  EXPECT_FALSE(aliases.isExact(P, R));
  EXPECT_FALSE(aliases.isExact(Q, R)) << "interior through r";
  EXPECT_TRUE(aliases.mayAlias(Q, R));

  // `if (p != r)`: r still points into p's object.
  aliases.separateExact(P, R);
  EXPECT_TRUE(aliases.mayAlias(P, R));
  // `if (p != q)`: refuted.
  aliases.separateExact(P, Q);
  EXPECT_FALSE(aliases.mayAlias(P, Q));
  EXPECT_TRUE(aliases.mayAlias(Q, R)) << "only the p-q edge goes";
  EXPECT_EQ(aliases.members(P), (Members{P, R}));

  // Joining an exact and an interior claim about one edge is interior.
  AliasRelation left;
  left.unite(Q, P);
  AliasRelation right;
  right.unite(Q, P, PointerOffset::unknown());
  left.join(right);
  EXPECT_FALSE(left.isExact(P, Q));
  EXPECT_TRUE(left.mayAlias(P, Q));
  AliasRelation again = left;
  again.join(right);
  EXPECT_EQ(again, left);
}

// RFC 0011, *Derived pointers*: edges carry offsets, which compose along
// the copies and cancel where `container_of` undoes a field step.
TEST(AliasRelation, OffsetsComposeAlongEdges) {
  const PointerOffset in = PointerOffset::ofField("struct outer .in");
  AliasRelation aliases;
  aliases.unite(Q, P, in);                         // q = &p->in
  EXPECT_EQ(aliases.offsetOf(P, Q), in);           // q is p + in
  EXPECT_EQ(aliases.offsetOf(Q, P), in.negated()); // p is q - in
  EXPECT_FALSE(aliases.isExact(P, Q));

  aliases.unite(R, Q, in.negated()); // r = container_of(q)
  EXPECT_TRUE(aliases.isExact(P, R)) << "the field step cancels";
  EXPECT_EQ(aliases.offsetOf(R, Q), in);

  // Element steps add up; `!=` refutes only an exact edge.
  aliases.unite(S, R, PointerOffset::ofElements(2)); // s = r + 2
  EXPECT_EQ(aliases.offsetOf(P, S), PointerOffset::ofElements(2));
  aliases.separateExact(P, S);
  EXPECT_TRUE(aliases.mayAlias(P, S));
  aliases.separateExact(P, R);
  EXPECT_FALSE(aliases.mayAlias(P, R));

  // `p++` moves `p` away from its aliases: `q = p; p++` leaves `q` one
  // before `p`.
  AliasRelation stepped;
  stepped.unite(Q, P);
  stepped.shift(P, PointerOffset::ofElements(1));
  EXPECT_EQ(stepped.offsetOf(P, Q), PointerOffset::ofElements(-1));
  EXPECT_EQ(stepped.offsetOf(Q, P), PointerOffset::ofElements(1));
  stepped.shift(P, PointerOffset::ofElements(-1));
  EXPECT_TRUE(stepped.isExact(P, Q));

  // Two claims about one edge join to what covers both: a field step and
  // an element step to somewhere inside the object.
  AliasRelation other;
  other.unite(Q, P, PointerOffset::ofElements(1));
  EXPECT_TRUE(aliases.join(other));
  EXPECT_EQ(aliases.offsetOf(P, Q), PointerOffset::inside());
  EXPECT_TRUE(aliases.mayAlias(P, Q));
}

// RFC 0006, *Element witnesses*: edges remember which element they name.
TEST(AliasRelation, ElementWitnessesOnEdges) {
  const ElementWitness zero = ElementWitness::ofConstant(0);
  const ElementWitness one = ElementWitness::ofConstant(1);
  AliasRelation aliases;
  // q = a[0]; r = a[1]  (P stands for a[*]).
  aliases.unite(Q, P, PointerOffset::zero(), ElementWitness::whole(), zero);
  aliases.unite(R, P, PointerOffset::zero(), ElementWitness::whole(), one);
  ASSERT_TRUE(aliases.edge(Q, P));
  EXPECT_EQ(aliases.edge(Q, P)->element, zero) << "q holds element 0 of a";
  EXPECT_TRUE(aliases.edge(P, Q)->element.isWhole()) << "a[0] holds all of q";
  EXPECT_EQ(aliases.edge(R, P)->element, one);
  EXPECT_FALSE(aliases.mayAlias(Q, R)) << "different elements: no alias";

  // s = a[0]: the same element as q, so s and q alias.
  aliases.unite(S, P, PointerOffset::zero(), ElementWitness::whole(), zero);
  EXPECT_TRUE(aliases.mayAlias(S, Q));
  EXPECT_FALSE(aliases.mayAlias(S, R));
  EXPECT_TRUE(aliases.edge(S, Q)->element.isWhole());

  // A whole copy of the array pointer reaches every element's alias.
  const PlaceId B{4};
  aliases.unite(B, P);
  EXPECT_TRUE(aliases.mayAlias(B, Q));
  EXPECT_TRUE(aliases.mayAlias(B, R));
  EXPECT_EQ(aliases.edge(Q, B)->element, zero)
      << "q is element 0 of b as it is of a";

  // Disagreeing witnesses on a join are unknown.
  AliasRelation left;
  left.unite(Q, P, PointerOffset::zero(), ElementWitness::whole(), zero);
  AliasRelation right;
  right.unite(Q, P, PointerOffset::zero(), ElementWitness::whole(), one);
  left.join(right);
  EXPECT_EQ(left.edge(Q, P)->element, ElementWitness::unknown());
  EXPECT_EQ(aliases.edgesFrom(Q).size(), 3U);
}

// RFC 0010, *Distinct shares*: a copy that takes a share of its own aliases
// its source (a free through either kills both) without holding the same
// share (a release through one leaves the other's).
TEST(AliasRelation, DistinctShareEdges) {
  AliasRelation aliases;
  aliases.unite(Q, P);
  EXPECT_TRUE(aliases.sameShare(P, Q)) << "a plain copy shares the share";
  EXPECT_TRUE(aliases.sameShare(P, P));
  EXPECT_TRUE(aliases.sameShare(P, R)) << "unrelated places: vacuously true";

  aliases.unite(R, P, PointerOffset::zero(), ElementWitness::whole(),
                ElementWitness::whole(), /*sameShare=*/false);
  EXPECT_TRUE(aliases.mayAlias(R, P));
  EXPECT_FALSE(aliases.sameShare(R, P));
  EXPECT_FALSE(aliases.sameShare(P, R)) << "symmetric";
  EXPECT_FALSE(aliases.sameShare(R, Q))
      << "derived through p: r's share is not q's either";
  EXPECT_TRUE(aliases.sameShare(P, Q)) << "the existing edge is untouched";

  // s = r: a plain copy of r holds r's share, and so not p's.
  aliases.unite(S, R);
  EXPECT_TRUE(aliases.sameShare(S, R));
  EXPECT_FALSE(aliases.sameShare(S, P));

  // Joins: an edge that is the same share on either side may be.
  AliasRelation left;
  left.unite(Q, P, PointerOffset::zero(), ElementWitness::whole(),
             ElementWitness::whole(), false);
  AliasRelation right;
  right.unite(Q, P);
  EXPECT_TRUE(left.join(right));
  EXPECT_TRUE(left.sameShare(Q, P));
  AliasRelation both;
  both.unite(Q, P, PointerOffset::zero(), ElementWitness::whole(),
             ElementWitness::whole(), false);
  AliasRelation same;
  same.unite(Q, P, PointerOffset::zero(), ElementWitness::whole(),
             ElementWitness::whole(), false);
  EXPECT_FALSE(both.join(same)) << "two distinct-share edges: unchanged";
  EXPECT_FALSE(both.sameShare(Q, P));
}

// `q = c ? p : r`: the arms are alternatives, so `p` and `r` do not come out
// related, exactly as if the two assignments had been joined (RFC 0002,
// *Alias relation*). `q` is still related to each arm and to its aliases.
TEST(AliasRelation, AlternativeArmsAreNotRelatedToEachOther) {
  AliasRelation aliases;
  aliases.unite(S, P); // s = p
  aliases.unite(Q, P, PointerOffset::zero(), ElementWitness::whole(),
                ElementWitness::whole(), /*sameShare=*/true,
                /*alternative=*/true);
  aliases.unite(Q, R, PointerOffset::zero(), ElementWitness::whole(),
                ElementWitness::whole(), /*sameShare=*/true,
                /*alternative=*/true);
  EXPECT_TRUE(aliases.mayAlias(Q, P));
  EXPECT_TRUE(aliases.mayAlias(Q, S)) << "through p's own aliases";
  EXPECT_TRUE(aliases.mayAlias(Q, R));
  EXPECT_FALSE(aliases.mayAlias(P, R)) << "never the same value on any path";
  EXPECT_FALSE(aliases.mayAlias(S, R));

  AliasRelation joined;
  joined.unite(S, P);
  joined.unite(Q, P);
  AliasRelation other;
  other.unite(S, P);
  other.unite(Q, R);
  joined.join(other);
  EXPECT_EQ(aliases, joined) << "the join of the per-arm relations";
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
