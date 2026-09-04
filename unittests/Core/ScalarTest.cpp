//===- ScalarTest.cpp - Tests for value facts, guards and scalars ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0009, *Value facts*, *Guards* and *Scalar facts in the state*.
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Scalar.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

// -- Value facts --------------------------------------------------------------

TEST(ValueFact, ConstantsKnowTheirClass) {
  EXPECT_EQ(ValueFact::ofConstant(0).classes, (OutcomeSet{Outcome::Zero}));
  EXPECT_EQ(ValueFact::ofConstant(3).classes, (OutcomeSet{Outcome::Positive}));
  EXPECT_EQ(ValueFact::ofConstant(-1).classes, (OutcomeSet{Outcome::Negative}));
  EXPECT_EQ(ValueFact::ofConstant(3).constant, 3);
}

TEST(ValueFact, TrivialFactsExcludeNothing) {
  EXPECT_TRUE(ValueFact::anyInteger().trivial());
  EXPECT_TRUE(ValueFact::anyPointer().trivial());
  EXPECT_FALSE(ValueFact::nonZero().trivial());
  EXPECT_FALSE(ValueFact::of(Outcome::Null).trivial());
  EXPECT_FALSE(ValueFact::ofConstant(0).trivial());
}

TEST(ValueFact, DisjointnessAndImplication) {
  const ValueFact zero = ValueFact::of(Outcome::Zero);
  const ValueFact nonZero = ValueFact::nonZero();
  const ValueFact positive = ValueFact::of(Outcome::Positive);
  const ValueFact three = ValueFact::ofConstant(3);
  const ValueFact four = ValueFact::ofConstant(4);

  EXPECT_TRUE(zero.disjointFrom(nonZero));
  EXPECT_FALSE(positive.disjointFrom(nonZero));
  // Two constants of one class are disjoint values.
  EXPECT_TRUE(three.disjointFrom(four));
  EXPECT_FALSE(three.disjointFrom(positive));

  EXPECT_TRUE(positive.implies(nonZero));
  EXPECT_FALSE(nonZero.implies(positive));
  EXPECT_TRUE(three.implies(positive));
  EXPECT_FALSE(positive.implies(three));
  EXPECT_TRUE(three.implies(three));
}

TEST(ValueFact, JoinUnitesClassesAndKeepsAgreedConstant) {
  ValueFact a = ValueFact::ofConstant(3);
  a.join(ValueFact::ofConstant(3));
  EXPECT_EQ(a, ValueFact::ofConstant(3));

  a.join(ValueFact::ofConstant(4));
  EXPECT_EQ(a.classes, (OutcomeSet{Outcome::Positive}));
  EXPECT_FALSE(a.constant);

  a.join(ValueFact::of(Outcome::Zero));
  EXPECT_EQ(a.classes, (OutcomeSet{Outcome::Zero, Outcome::Positive}));
  a.join(ValueFact::of(Outcome::Negative));
  EXPECT_TRUE(a.trivial());
}

TEST(ValueFact, NarrowIntersectsAndRefutes) {
  ValueFact a = ValueFact::nonZero();
  EXPECT_TRUE(a.narrow(ValueFact::of({Outcome::Zero, Outcome::Positive})));
  EXPECT_EQ(a, ValueFact::of(Outcome::Positive));
  // The side with a constant supplies it.
  EXPECT_TRUE(a.narrow(ValueFact::ofConstant(7)));
  EXPECT_EQ(a, ValueFact::ofConstant(7));
  // Disagreeing constants, or no class in common: refuted and unchanged.
  EXPECT_FALSE(a.narrow(ValueFact::ofConstant(8)));
  EXPECT_FALSE(a.narrow(ValueFact::of(Outcome::Zero)));
  EXPECT_EQ(a, ValueFact::ofConstant(7));
}

TEST(ValueFact, TextRoundTrips) {
  for (const ValueFact &fact :
       {ValueFact::ofConstant(0), ValueFact::ofConstant(-12),
        ValueFact::ofConstant(42), ValueFact::nonZero(),
        ValueFact::of(Outcome::Null), ValueFact::of(Outcome::NonNull),
        ValueFact::of({Outcome::Zero, Outcome::Negative})}) {
    const auto parsed = ValueFact::parse(fact.toString());
    ASSERT_TRUE(parsed) << fact.toString();
    EXPECT_EQ(*parsed, fact) << fact.toString();
  }
  EXPECT_EQ(ValueFact::ofConstant(3).toString(), "=3");
  EXPECT_EQ(ValueFact::nonZero().toString(), "positive|negative");
  EXPECT_FALSE(ValueFact::parse(""));
  EXPECT_FALSE(ValueFact::parse("maybe"));
  EXPECT_FALSE(ValueFact::parse("=x"));
}

// -- Guards -------------------------------------------------------------------

TEST(Guard, RequireNarrowsAndDropsContradictions) {
  PlaceGuard guard;
  EXPECT_TRUE(guard.trivial());
  EXPECT_FALSE(guard.require(PlaceId{1}, ValueFact::anyInteger()))
      << "a trivial fact is no condition";
  EXPECT_TRUE(guard.require(PlaceId{1}, ValueFact::nonZero()));
  // Already implied: unchanged.
  EXPECT_FALSE(guard.require(PlaceId{1}, ValueFact::nonZero()));
  // Narrowed to the intersection.
  EXPECT_TRUE(guard.require(PlaceId{1}, ValueFact::of(Outcome::Positive)));
  EXPECT_EQ(guard.conditions.at(PlaceId{1}), ValueFact::of(Outcome::Positive));
  // A contradiction cannot be expressed; the conjunct goes (weaker).
  EXPECT_TRUE(guard.require(PlaceId{1}, ValueFact::of(Outcome::Zero)));
  EXPECT_TRUE(guard.trivial());
}

TEST(Guard, RefineDischargesNarrowsRefutes) {
  PlaceGuard guard;
  guard.require(PlaceId{1}, ValueFact::of({Outcome::Zero, Outcome::Positive}));
  guard.require(PlaceId{2}, ValueFact::of(Outcome::Null));

  EXPECT_EQ(guard.refine(PlaceId{9}, ValueFact::of(Outcome::Zero)),
            GuardRefinement::Unchanged);
  // Overlapping but not contained: the conjunct is the intersection.
  EXPECT_EQ(guard.refine(PlaceId{1}, ValueFact::nonZero()),
            GuardRefinement::Narrowed);
  EXPECT_EQ(guard.conditions.at(PlaceId{1}), ValueFact::of(Outcome::Positive));
  // A fact that satisfies the conjunct: it is known to hold.
  EXPECT_EQ(guard.refine(PlaceId{1}, ValueFact::ofConstant(3)),
            GuardRefinement::Discharged);
  EXPECT_FALSE(guard.conditions.contains(PlaceId{1}));
  EXPECT_EQ(guard.refine(PlaceId{2}, ValueFact::of(Outcome::NonNull)),
            GuardRefinement::Refuted);
  // A refuted guard is left for the caller to act on.
  EXPECT_TRUE(guard.conditions.contains(PlaceId{2}));
}

TEST(Guard, LearnAddsTheEdgeFactUnlessDisjoint) {
  PlaceGuard guard;
  guard.require(PlaceId{1}, ValueFact::nonZero());
  EXPECT_EQ(guard.learn(PlaceId{2}, ValueFact::of(Outcome::Zero)),
            GuardRefinement::Narrowed);
  EXPECT_EQ(guard.conditions.size(), 2U);
  EXPECT_EQ(guard.learn(PlaceId{1}, ValueFact::of(Outcome::Zero)),
            GuardRefinement::Refuted);
  EXPECT_EQ(guard.learn(PlaceId{3}, ValueFact::anyInteger()),
            GuardRefinement::Unchanged);
}

TEST(Guard, JoinKeepsWhatBothSidesAgreeOn) {
  PlaceGuard a;
  a.require(PlaceId{1}, ValueFact::of(Outcome::Positive));
  a.require(PlaceId{2}, ValueFact::of(Outcome::Null));
  a.require(PlaceId{3}, ValueFact::ofConstant(3));
  PlaceGuard b;
  b.require(PlaceId{1}, ValueFact::of(Outcome::Negative));
  b.require(PlaceId{3}, ValueFact::ofConstant(3));

  EXPECT_TRUE(a.join(b));
  // Key 2 was on one side only; key 1 is the union of the classes; key 3
  // agreed exactly.
  EXPECT_FALSE(a.conditions.contains(PlaceId{2}));
  EXPECT_EQ(a.conditions.at(PlaceId{1}), ValueFact::nonZero());
  EXPECT_EQ(a.conditions.at(PlaceId{3}), ValueFact::ofConstant(3));

  // A joined fact that excludes nothing is no conjunct.
  PlaceGuard c;
  c.require(PlaceId{1}, ValueFact::of(Outcome::Zero));
  EXPECT_TRUE(a.join(c));
  EXPECT_FALSE(a.conditions.contains(PlaceId{1}));
  EXPECT_TRUE(a.trivial());
  EXPECT_FALSE(a.join(c));
}

TEST(Guard, DropRemovesOneConjunct) {
  PlaceGuard guard;
  guard.require(PlaceId{1}, ValueFact::nonZero());
  guard.require(PlaceId{2}, ValueFact::nonZero());
  EXPECT_TRUE(guard.drop(PlaceId{1}));
  EXPECT_FALSE(guard.drop(PlaceId{1}));
  EXPECT_EQ(guard.conditions.size(), 1U);
}

TEST(Guard, IsBounded) {
  PlaceGuard guard;
  for (std::uint32_t i = 0; i < MaxGuardConjuncts + 4; ++i)
    guard.require(PlaceId{i}, ValueFact::nonZero());
  EXPECT_EQ(guard.conditions.size(), MaxGuardConjuncts);
  // A full guard still narrows the keys it has.
  EXPECT_TRUE(guard.require(PlaceId{0}, ValueFact::of(Outcome::Positive)));
}

// -- Scalar tracker -----------------------------------------------------------

TEST(ScalarTracker, SetNarrowForget) {
  ScalarTracker tracker;
  EXPECT_TRUE(tracker.empty());
  tracker.set(PlaceId{1}, ValueFact::ofConstant(0));
  EXPECT_EQ(tracker.factOf(PlaceId{1}), ValueFact::ofConstant(0));
  // A trivial fact is no entry.
  tracker.set(PlaceId{2}, ValueFact::anyInteger());
  EXPECT_FALSE(tracker.factOf(PlaceId{2}));

  EXPECT_EQ(tracker.narrow(PlaceId{1}, ValueFact::nonZero()),
            GuardRefinement::Refuted);
  EXPECT_EQ(tracker.factOf(PlaceId{1}), ValueFact::ofConstant(0))
      << "a refuted edge changes nothing";
  EXPECT_EQ(tracker.narrow(PlaceId{1}, ValueFact::of(Outcome::Zero)),
            GuardRefinement::Unchanged)
      << "already implied";
  EXPECT_EQ(tracker.narrow(PlaceId{1}, ValueFact::anyInteger()),
            GuardRefinement::Unchanged);
  // Narrowing an unknown place records the edge's fact.
  EXPECT_EQ(tracker.narrow(PlaceId{3}, ValueFact::of(Outcome::Positive)),
            GuardRefinement::Narrowed);
  EXPECT_EQ(tracker.factOf(PlaceId{3}), ValueFact::of(Outcome::Positive));

  tracker.forget(PlaceId{1});
  EXPECT_FALSE(tracker.factOf(PlaceId{1}));
  EXPECT_EQ(tracker.places(), (std::vector{PlaceId{3}}));
}

TEST(ScalarTracker, JoinIntersectsDomainsAndJoinsFacts) {
  ScalarTracker a;
  a.set(PlaceId{1}, ValueFact::ofConstant(0));
  a.set(PlaceId{2}, ValueFact::ofConstant(5));
  a.set(PlaceId{3}, ValueFact::of(Outcome::Positive));
  ScalarTracker b;
  b.set(PlaceId{1}, ValueFact::ofConstant(0));
  b.set(PlaceId{2}, ValueFact::ofConstant(6));
  b.set(PlaceId{4}, ValueFact::of(Outcome::Zero));

  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.factOf(PlaceId{1}), ValueFact::ofConstant(0));
  EXPECT_EQ(a.factOf(PlaceId{2}), ValueFact::of(Outcome::Positive));
  EXPECT_FALSE(a.factOf(PlaceId{3})) << "known on one side only";
  EXPECT_FALSE(a.factOf(PlaceId{4}));
  EXPECT_FALSE(a.join(b)) << "idempotent";

  // A join that covers every class drops the entry.
  ScalarTracker c;
  c.set(PlaceId{2}, ValueFact::of({Outcome::Zero, Outcome::Negative}));
  EXPECT_TRUE(a.join(c));
  EXPECT_FALSE(a.factOf(PlaceId{2}));
}

} // namespace
} // namespace weavec::core
