//===- MovesTest.cpp - Tests for move tracking ----------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Moves.h"

#include <gtest/gtest.h>

namespace weavec::core {

static SourceLocation at(std::uint32_t line) {
  return SourceLocation{.file = "t.c", .line = line, .column = 1, .opaque = 0};
}

namespace {

TEST(MoveTracker, FreshPlaceIsNotMoved) {
  MoveTracker tracker;
  EXPECT_FALSE(tracker.isMoved(PlaceId{0}));
  EXPECT_FALSE(tracker.movedAt(PlaceId{0}));
}

TEST(MoveTracker, MarkMovedRecordsReasonAndLocation) {
  MoveTracker tracker;
  EXPECT_FALSE(tracker.markMoved(PlaceId{0}, MoveReason::Freed, at(10)));
  const auto record = tracker.movedAt(PlaceId{0});
  ASSERT_TRUE(record);
  EXPECT_EQ(record->reason, MoveReason::Freed);
  EXPECT_EQ(record->location.line, 10U);
}

TEST(MoveTracker, DoubleMoveReturnsOriginalRecord) {
  MoveTracker tracker;
  ASSERT_FALSE(tracker.markMoved(PlaceId{0}, MoveReason::Freed, at(10)));
  const auto previous =
      tracker.markMoved(PlaceId{0}, MoveReason::Freed, at(20));
  ASSERT_TRUE(previous);
  EXPECT_EQ(previous->location.line, 10U);
  // The first record wins so later diagnostics point at the first site.
  EXPECT_EQ(tracker.movedAt(PlaceId{0})->location.line, 10U);
}

TEST(MoveTracker, ReinitializeClearsState) {
  MoveTracker tracker;
  ASSERT_FALSE(tracker.markMoved(PlaceId{0}, MoveReason::Moved, at(1)));
  tracker.reinitialize(PlaceId{0});
  EXPECT_FALSE(tracker.isMoved(PlaceId{0}));
  EXPECT_FALSE(tracker.markMoved(PlaceId{0}, MoveReason::Moved, at(2)));
}

TEST(MoveTracker, JoinIsUnion) {
  MoveTracker a;
  MoveTracker b;
  ASSERT_FALSE(a.markMoved(PlaceId{0}, MoveReason::Freed, at(1)));
  ASSERT_FALSE(b.markMoved(PlaceId{1}, MoveReason::Moved, at(2)));
  ASSERT_FALSE(b.markMoved(PlaceId{0}, MoveReason::Freed, at(3)));

  a.join(b);
  EXPECT_TRUE(a.isMoved(PlaceId{0}));
  EXPECT_TRUE(a.isMoved(PlaceId{1}));
  EXPECT_EQ(a.movedAt(PlaceId{0})->location.line, 1U)
      << "existing records are preserved on join";
  EXPECT_FALSE(a.isMoved(PlaceId{2}));
}

TEST(MoveTracker, OwnValueIsAMustFactAcrossJoins) {
  // RFC 0008, *Replaced values*: a record for the function's own value stays
  // so only while every path agrees; a path where the record may be the
  // caller's wins.
  MoveTracker own;
  ASSERT_FALSE(own.markMoved(PlaceId{0}, MoveReason::Freed, at(1), {},
                             ElementWitness::whole(), "free",
                             /*ownValue=*/true));
  EXPECT_TRUE(own.movedAt(PlaceId{0})->ownValue);

  MoveTracker alsoOwn = own;
  EXPECT_FALSE(alsoOwn.join(own)) << "identical records: no change";
  EXPECT_TRUE(alsoOwn.movedAt(PlaceId{0})->ownValue);

  MoveTracker callers;
  ASSERT_FALSE(callers.markMoved(PlaceId{0}, MoveReason::Freed, at(2)));
  EXPECT_TRUE(own.join(callers));
  EXPECT_FALSE(own.movedAt(PlaceId{0})->ownValue);

  MoveTracker untouched;
  MoveTracker ownAgain;
  ASSERT_FALSE(ownAgain.markMoved(PlaceId{0}, MoveReason::Freed, at(1), {},
                                  ElementWitness::whole(), "free",
                                  /*ownValue=*/true));
  EXPECT_TRUE(ownAgain.join(untouched) == false)
      << "the other side has no record: nothing to learn";
  EXPECT_TRUE(ownAgain.movedAt(PlaceId{0})->ownValue)
      << "a path that never touched the place says nothing about whose "
         "value was consumed";
}

TEST(MoveTracker, RecordsTheAliasTheMoveWentThrough) {
  MoveTracker tracker;
  ASSERT_FALSE(tracker.markMoved(PlaceId{0}, MoveReason::Freed, at(1)));
  ASSERT_FALSE(
      tracker.markMoved(PlaceId{1}, MoveReason::Freed, at(1), PlaceId{0}));
  EXPECT_FALSE(tracker.movedAt(PlaceId{0})->via);
  EXPECT_EQ(tracker.movedAt(PlaceId{1})->via, PlaceId{0});
}

// RFC 0006, *Element witnesses*.
TEST(ElementWitness, MatchingRules) {
  const ElementWitness whole = ElementWitness::whole();
  const ElementWitness zero = ElementWitness::ofConstant(0);
  const ElementWitness one = ElementWitness::ofConstant(1);
  const ElementWitness i = ElementWitness::ofVariable(PlaceId{7});
  const ElementWitness j = ElementWitness::ofVariable(PlaceId{8});
  const ElementWitness unknown = ElementWitness::unknown();

  EXPECT_TRUE(whole.matches(zero));
  EXPECT_TRUE(zero.matches(whole));
  EXPECT_TRUE(whole.matches(unknown));
  EXPECT_TRUE(unknown.matches(whole));
  EXPECT_TRUE(zero.matches(zero));
  EXPECT_FALSE(zero.matches(one));
  EXPECT_TRUE(i.matches(i));
  EXPECT_FALSE(i.matches(j));
  EXPECT_FALSE(i.matches(zero));
  EXPECT_FALSE(unknown.matches(unknown))
      << "an unknown element is nothing in particular";
  EXPECT_FALSE(unknown.matches(i));
}

TEST(MoveTracker, ElementWitnessesDistinguishElements) {
  const PlaceId a{0};
  const ElementWitness i = ElementWitness::ofVariable(PlaceId{5});
  MoveTracker tracker;
  // free(a[i])
  EXPECT_FALSE(tracker.markMoved(a, MoveReason::Freed, at(1), {}, i));
  EXPECT_TRUE(tracker.movedAt(a, i)) << "a[i] again: the same element";
  EXPECT_TRUE(tracker.movedAt(a)) << "a whole access sees any record";
  EXPECT_FALSE(tracker.movedAt(a, ElementWitness::ofConstant(0)))
      << "a[0] is not known to be a[i]";
  EXPECT_FALSE(tracker.movedAt(a, ElementWitness::ofVariable(PlaceId{6})));

  // free(a[i]) again: a double free.
  const auto again = tracker.markMoved(a, MoveReason::Freed, at(2), {}, i);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->location.line, 1U);

  // free(a[j]): another element replaces the record without complaint.
  const ElementWitness j = ElementWitness::ofVariable(PlaceId{6});
  EXPECT_FALSE(tracker.markMoved(a, MoveReason::Freed, at(3), {}, j));
  EXPECT_EQ(tracker.recordOf(a)->element, j);
  EXPECT_FALSE(tracker.movedAt(a, i));

  // a[i] = NULL does not reinitialise a[j]; a[j] = NULL does.
  tracker.reinitialize(a, i);
  EXPECT_TRUE(tracker.movedAt(a, j));
  tracker.reinitialize(a, j);
  EXPECT_FALSE(tracker.isMoved(a));

  // i++ turns a record witnessed by i into an unknown element.
  EXPECT_FALSE(tracker.markMoved(a, MoveReason::Freed, at(4), {}, i));
  tracker.forgetWitness(PlaceId{5});
  EXPECT_EQ(tracker.recordOf(a)->element, ElementWitness::unknown());
  EXPECT_FALSE(tracker.movedAt(a, i)) << "stale: the loop moved on";
  EXPECT_TRUE(tracker.movedAt(a)) << "but some element is freed";
  EXPECT_FALSE(tracker.markMoved(a, MoveReason::Freed, at(5), {}, i))
      << "the next iteration's free is not a double free";
}

TEST(MoveTracker, JoinOfDifferentWitnesses) {
  const PlaceId a{0};
  const ElementWitness zero = ElementWitness::ofConstant(0);
  const ElementWitness one = ElementWitness::ofConstant(1);
  MoveTracker left;
  MoveTracker right;
  ASSERT_FALSE(left.markMoved(a, MoveReason::Freed, at(1), {}, zero));
  ASSERT_FALSE(right.markMoved(a, MoveReason::Freed, at(2), {}, one));
  left.join(right);
  EXPECT_EQ(left.recordOf(a)->element, ElementWitness::unknown());

  MoveTracker whole;
  ASSERT_FALSE(whole.markMoved(a, MoveReason::Freed, at(3)));
  MoveTracker joined = left;
  joined.join(whole);
  EXPECT_TRUE(joined.recordOf(a)->element.isWhole())
      << "a whole-place move on either side covers every element";
  MoveTracker same = left;
  same.join(left);
  EXPECT_EQ(same, left);
}

TEST(MoveTracker, EqualityAndOrderedListing) {
  MoveTracker a;
  MoveTracker b;
  EXPECT_EQ(a, b);
  ASSERT_FALSE(a.markMoved(PlaceId{2}, MoveReason::Freed, at(1)));
  ASSERT_FALSE(a.markMoved(PlaceId{0}, MoveReason::Freed, at(1)));
  EXPECT_NE(a, b);
  ASSERT_FALSE(b.markMoved(PlaceId{0}, MoveReason::Freed, at(1)));
  ASSERT_FALSE(b.markMoved(PlaceId{2}, MoveReason::Freed, at(1)));
  EXPECT_EQ(a, b) << "insertion order is irrelevant";
  EXPECT_EQ(a.movedPlaces(), (std::vector<PlaceId>{PlaceId{0}, PlaceId{2}}));
}

// RFC 0008, *Uninitialised pointers*: a place that never held a value is a
// moved place whose reason says so; the first write reinitialises it.
TEST(MoveTracker, UninitializedIsAMoveReason) {
  MoveTracker tracker;
  ASSERT_FALSE(tracker.markMoved(PlaceId{0}, MoveReason::Uninitialized, at(4)));
  EXPECT_TRUE(tracker.isMoved(PlaceId{0}));
  EXPECT_EQ(tracker.movedAt(PlaceId{0})->reason, MoveReason::Uninitialized);
  tracker.reinitialize(PlaceId{0});
  EXPECT_FALSE(tracker.isMoved(PlaceId{0}));

  // A join with a path that initialised it keeps the record: it may be
  // uninitialised at the merge.
  MoveTracker initialised;
  ASSERT_FALSE(tracker.markMoved(PlaceId{1}, MoveReason::Uninitialized, at(4)));
  tracker.join(initialised);
  EXPECT_TRUE(tracker.isMoved(PlaceId{1}));

  EXPECT_EQ(toString(MoveReason::Moved), "moved");
  EXPECT_EQ(toString(MoveReason::Freed), "freed");
  EXPECT_EQ(toString(MoveReason::Uninitialized), "uninitialized");
}

} // namespace
} // namespace weavec::core
