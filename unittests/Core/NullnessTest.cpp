//===- NullnessTest.cpp - Tests for nullness tracking ---------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0008, *Nullness*.
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Nullness.h"

#include <gtest/gtest.h>

namespace weavec::core {

static SourceLocation at(std::uint32_t line) {
  return SourceLocation{.file = "t.c", .line = line, .column = 1, .opaque = 0};
}

static NullRecord record(Nullness state, std::uint32_t line,
                         NullReason reason = NullReason::AssignedNull,
                         std::string detail = {}) {
  return NullRecord{.state = state,
                    .location = at(line),
                    .reason = reason,
                    .detail = std::move(detail)};
}

namespace {

constexpr PlaceId P{1};
constexpr PlaceId Q{2};

TEST(NullTracker, FreshTrackerKnowsNothing) {
  const NullTracker tracker;
  EXPECT_TRUE(tracker.empty());
  EXPECT_FALSE(tracker.recordOf(P));
  EXPECT_FALSE(tracker.stateOf(P));
  EXPECT_FALSE(tracker.mayBeNull(P)) << "no fact is trusted, not may-null";
  EXPECT_FALSE(tracker.isNonNull(P));
  EXPECT_TRUE(tracker.places().empty());
}

TEST(NullTracker, SetReplacesAndForgetDrops) {
  NullTracker tracker;
  tracker.set(P, record(Nullness::Null, 3));
  EXPECT_EQ(tracker.stateOf(P), Nullness::Null);
  EXPECT_TRUE(tracker.mayBeNull(P));
  EXPECT_FALSE(tracker.isNonNull(P));
  ASSERT_TRUE(tracker.recordOf(P));
  EXPECT_EQ(tracker.recordOf(P)->location.line, 3U);
  EXPECT_TRUE(tracker.recordOf(P)->mayBeNull());

  tracker.set(P, record(Nullness::NonNull, 5, NullReason::Tested));
  EXPECT_EQ(tracker.stateOf(P), Nullness::NonNull);
  EXPECT_TRUE(tracker.isNonNull(P));
  EXPECT_FALSE(tracker.mayBeNull(P));
  EXPECT_FALSE(tracker.recordOf(P)->mayBeNull());
  EXPECT_EQ(tracker.recordOf(P)->reason, NullReason::Tested);

  tracker.forget(P);
  EXPECT_FALSE(tracker.stateOf(P));
  EXPECT_TRUE(tracker.empty());
}

TEST(NullTracker, RecordsCarryTheirProvenance) {
  NullTracker tracker;
  tracker.set(
      P, record(Nullness::MaybeNull, 7, NullReason::CalleeResult, "'malloc'"));
  const auto rec = tracker.recordOf(P);
  ASSERT_TRUE(rec);
  EXPECT_EQ(rec->reason, NullReason::CalleeResult);
  EXPECT_EQ(rec->detail, "'malloc'");
  EXPECT_EQ(rec->location.line, 7U);
}

// -- The join table (RFC 0008, *Nullness*) ------------------------------------

TEST(NullTracker, JoinOfEqualStatesIsUnchanged) {
  NullTracker a;
  NullTracker b;
  a.set(P, record(Nullness::Null, 1));
  b.set(P, record(Nullness::Null, 9));
  EXPECT_FALSE(a.join(b));
  EXPECT_EQ(a.stateOf(P), Nullness::Null);
  EXPECT_EQ(a.recordOf(P)->location.line, 1U) << "this side's record is kept";
}

TEST(NullTracker, NullWithNonNullIsMaybeNullKeepingTheNullRecord) {
  NullTracker a;
  NullTracker b;
  a.set(P, record(Nullness::NonNull, 1, NullReason::Tested));
  b.set(P, record(Nullness::Null, 9));
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.stateOf(P), Nullness::MaybeNull);
  EXPECT_EQ(a.recordOf(P)->location.line, 9U) << "the side that said null";
  EXPECT_EQ(a.recordOf(P)->reason, NullReason::AssignedNull);

  // And the other way round: this side said null, its record stays.
  NullTracker c;
  NullTracker d;
  c.set(P, record(Nullness::Null, 2));
  d.set(P, record(Nullness::NonNull, 8, NullReason::Tested));
  EXPECT_TRUE(c.join(d));
  EXPECT_EQ(c.stateOf(P), Nullness::MaybeNull);
  EXPECT_EQ(c.recordOf(P)->location.line, 2U);
}

TEST(NullTracker, MaybeNullAbsorbsEverything) {
  NullTracker a;
  NullTracker b;
  a.set(P, record(Nullness::MaybeNull, 1));
  b.set(P, record(Nullness::NonNull, 9, NullReason::Tested));
  EXPECT_FALSE(a.join(b));
  EXPECT_EQ(a.stateOf(P), Nullness::MaybeNull);

  NullTracker c;
  NullTracker d;
  c.set(P, record(Nullness::MaybeNull, 1));
  d.set(P, record(Nullness::Null, 9));
  EXPECT_FALSE(c.join(d));
  EXPECT_EQ(c.stateOf(P), Nullness::MaybeNull);
  EXPECT_EQ(c.recordOf(P)->location.line, 1U);
}

TEST(NullTracker, NonNullWithNoFactIsNoFact) {
  // A `NonNull` fact on one path and nothing on the other: nothing is known
  // at the join, on either side.
  NullTracker a;
  NullTracker b;
  a.set(P, record(Nullness::NonNull, 1, NullReason::Tested));
  EXPECT_TRUE(a.join(b));
  EXPECT_FALSE(a.stateOf(P));

  NullTracker c;
  NullTracker d;
  d.set(P, record(Nullness::NonNull, 1, NullReason::Tested));
  EXPECT_FALSE(c.join(d));
  EXPECT_FALSE(c.stateOf(P));
}

TEST(NullTracker, NullWithNoFactIsMaybeNull) {
  // Some path assigned null: the value may be null after the join even if
  // the other path knew nothing (the unknown side could be anything).
  NullTracker a;
  NullTracker b;
  b.set(P, record(Nullness::Null, 4));
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.stateOf(P), Nullness::MaybeNull);
  EXPECT_EQ(a.recordOf(P)->location.line, 4U);

  NullTracker c;
  NullTracker d;
  c.set(P, record(Nullness::Null, 4));
  EXPECT_TRUE(c.join(d));
  EXPECT_EQ(c.stateOf(P), Nullness::MaybeNull);
}

TEST(NullTracker, JoinIsPerPlace) {
  NullTracker a;
  NullTracker b;
  a.set(P, record(Nullness::Null, 1));
  a.set(Q, record(Nullness::NonNull, 2, NullReason::Tested));
  b.set(P, record(Nullness::Null, 1));
  b.set(Q, record(Nullness::NonNull, 2, NullReason::Tested));
  EXPECT_FALSE(a.join(b));
  EXPECT_EQ(a.places(), (std::vector<PlaceId>{P, Q}));
  EXPECT_EQ(a, b);
}

TEST(NullTracker, JoinIsIdempotentAndReachesAFixpoint) {
  NullTracker a;
  NullTracker b;
  a.set(P, record(Nullness::NonNull, 1, NullReason::Tested));
  b.set(P, record(Nullness::Null, 2));
  EXPECT_TRUE(a.join(b));
  const NullTracker once = a;
  EXPECT_FALSE(a.join(b));
  EXPECT_EQ(a, once);
  EXPECT_FALSE(a.join(a));
}

// -- Guards (RFC 0009) --------------------------------------------------------

static PlaceGuard when(PlaceId key, const ValueFact &fact) {
  PlaceGuard guard;
  guard.require(key, fact);
  return guard;
}

constexpr PlaceId C{3};

TEST(NullTracker, NullJoinedWithNonNullIsNullExactlyUnderItsGuard) {
  // `if (c) p = NULL; else p = &x;`: null when `c` is non-zero, non-null
  // otherwise. The `!c` edge makes the record `NonNull` again.
  NullTracker a;
  NullTracker b;
  NullRecord null = record(Nullness::Null, 2);
  null.guard = when(C, ValueFact::nonZero());
  a.set(P, null);
  b.set(P, record(Nullness::NonNull, 4));
  EXPECT_TRUE(a.join(b));
  ASSERT_TRUE(a.recordOf(P));
  EXPECT_EQ(a.recordOf(P)->state, Nullness::MaybeNull);
  EXPECT_TRUE(a.recordOf(P)->otherwiseNonNull);
  EXPECT_EQ(a.recordOf(P)->guard, when(C, ValueFact::nonZero()));

  EXPECT_EQ(a.learn(C, ValueFact::of(Outcome::Zero)),
            (std::vector<PlaceId>{P}));
  EXPECT_EQ(a.stateOf(P), Nullness::NonNull);
  EXPECT_TRUE(a.recordOf(P)->guard.trivial());
  EXPECT_FALSE(a.recordOf(P)->otherwiseNonNull);
}

TEST(NullTracker, NullJoinedWithNothingMakesNoPromise) {
  // The other path knew nothing about `p`: refuting the guard says nothing
  // about what it holds, so the record simply goes.
  NullTracker a;
  NullTracker b;
  NullRecord null = record(Nullness::Null, 2);
  null.guard = when(C, ValueFact::nonZero());
  a.set(P, null);
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.stateOf(P), Nullness::MaybeNull);
  EXPECT_FALSE(a.recordOf(P)->otherwiseNonNull);

  EXPECT_EQ(a.learn(C, ValueFact::of(Outcome::Zero)),
            (std::vector<PlaceId>{P}));
  EXPECT_FALSE(a.stateOf(P));
}

TEST(NullTracker, PromiseSurvivesOnlyWhenEverySideMadeIt) {
  NullRecord promised = record(Nullness::MaybeNull, 2);
  promised.guard = when(C, ValueFact::of(Outcome::Positive));
  promised.otherwiseNonNull = true;
  NullRecord unpromised = record(Nullness::MaybeNull, 5);
  unpromised.guard = when(C, ValueFact::of(Outcome::Negative));

  NullTracker a;
  a.set(P, promised);
  NullTracker b;
  b.set(P, unpromised);
  EXPECT_TRUE(a.join(b));
  EXPECT_EQ(a.recordOf(P)->guard, when(C, ValueFact::nonZero()));
  EXPECT_FALSE(a.recordOf(P)->otherwiseNonNull);

  // Two promising sides keep it; a `Null` side has no non-null paths and
  // keeps the other's promise.
  NullTracker c;
  c.set(P, promised);
  NullTracker d;
  NullRecord null = record(Nullness::Null, 7);
  null.guard = when(C, ValueFact::of(Outcome::Negative));
  d.set(P, null);
  EXPECT_TRUE(c.join(d));
  EXPECT_EQ(c.stateOf(P), Nullness::MaybeNull);
  EXPECT_TRUE(c.recordOf(P)->otherwiseNonNull);
}

TEST(NullTracker, LearnLeavesNonNullAloneAndDropWeakens) {
  NullTracker tracker;
  tracker.set(P, record(Nullness::NonNull, 1, NullReason::Tested));
  NullRecord null = record(Nullness::Null, 2);
  null.guard = when(C, ValueFact::nonZero());
  tracker.set(Q, null);

  // A compatible fact narrows the guard; `NonNull` records carry none.
  EXPECT_TRUE(tracker.learn(C, ValueFact::of(Outcome::Positive)).empty());
  EXPECT_TRUE(tracker.recordOf(P)->guard.trivial());
  EXPECT_EQ(tracker.recordOf(Q)->guard,
            when(C, ValueFact::of(Outcome::Positive)));

  tracker.dropGuardsOn(C);
  EXPECT_TRUE(tracker.recordOf(Q)->guard.trivial());
  EXPECT_EQ(tracker.stateOf(Q), Nullness::Null);
}

TEST(NullTracker, Names) {
  EXPECT_EQ(toString(Nullness::Null), "null");
  EXPECT_EQ(toString(Nullness::MaybeNull), "maybe-null");
  EXPECT_EQ(toString(Nullness::NonNull), "nonnull");
  EXPECT_EQ(toString(NullReason::AssignedNull), "assigned-null");
  EXPECT_EQ(toString(NullReason::CalleeResult), "callee-result");
  EXPECT_EQ(toString(NullReason::CalleeStore), "callee-store");
  EXPECT_EQ(toString(NullReason::Tested), "tested");
  EXPECT_EQ(toString(NullReason::Declared), "declared");
  EXPECT_EQ(toString(NullReason::Dereferenced), "dereferenced");
}

} // namespace
} // namespace weavec::core
