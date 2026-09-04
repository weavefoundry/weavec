//===- ResourceTest.cpp - Tests for owned resource tracking ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0007, *Resources (Core)*.
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Resource.h"

#include <gtest/gtest.h>

namespace weavec::core {

static SourceLocation at(std::uint32_t line) {
  return SourceLocation{.file = "t.c", .line = line, .column = 1, .opaque = 0};
}

static ResourceRecord allocated(std::uint32_t line, std::string family) {
  return ResourceRecord{.origin = ResourceOrigin::Allocated,
                        .location = at(line),
                        .family = std::move(family),
                        .escaped = false};
}

namespace {

TEST(ResourceTracker, FreshTrackerHoldsNothing) {
  const ResourceTracker tracker;
  EXPECT_TRUE(tracker.empty());
  EXPECT_FALSE(tracker.holds(PlaceId{0}));
  EXPECT_FALSE(tracker.recordOf(PlaceId{0}));
  EXPECT_FALSE(tracker.isEscaped(PlaceId{0}));
  EXPECT_FALSE(tracker.isNull(PlaceId{0}));
}

TEST(ResourceTracker, HoldRecordsOriginFamilyAndLocation) {
  ResourceTracker tracker;
  tracker.hold(PlaceId{1}, allocated(3, "fclose"));
  ASSERT_TRUE(tracker.holds(PlaceId{1}));
  const auto record = tracker.recordOf(PlaceId{1});
  ASSERT_TRUE(record);
  EXPECT_EQ(record->origin, ResourceOrigin::Allocated);
  EXPECT_EQ(record->family, "fclose");
  EXPECT_EQ(record->location.line, 3U);
  EXPECT_FALSE(record->escaped);
}

TEST(ResourceTracker, ReassignmentIsANewResource) {
  ResourceTracker tracker;
  tracker.hold(PlaceId{1}, allocated(3, "free"));
  tracker.escape(PlaceId{1});
  tracker.hold(PlaceId{1}, allocated(9, "fclose"));
  const auto record = tracker.recordOf(PlaceId{1});
  ASSERT_TRUE(record);
  EXPECT_EQ(record->location.line, 9U);
  EXPECT_EQ(record->family, "fclose");
  EXPECT_FALSE(record->escaped) << "the old record's escape does not carry";
}

TEST(ResourceTracker, EscapeNeedsARecord) {
  ResourceTracker tracker;
  tracker.escape(PlaceId{4});
  EXPECT_FALSE(tracker.holds(PlaceId{4}));
  EXPECT_FALSE(tracker.isEscaped(PlaceId{4}));
  tracker.hold(PlaceId{4}, allocated(1, "free"));
  tracker.escape(PlaceId{4});
  EXPECT_TRUE(tracker.isEscaped(PlaceId{4}));
  EXPECT_TRUE(tracker.holds(PlaceId{4})) << "escaped resources stay recorded";
}

TEST(ResourceTracker, ClearForgetsTheRecordOnly) {
  ResourceTracker tracker;
  tracker.hold(PlaceId{2}, allocated(1, "free"));
  tracker.clear(PlaceId{2});
  EXPECT_FALSE(tracker.holds(PlaceId{2}));
  EXPECT_TRUE(tracker.empty());
}

TEST(ResourceTracker, NullMarksDropTheRecordAndViceVersa) {
  ResourceTracker tracker;
  tracker.hold(PlaceId{2}, allocated(1, "free"));
  tracker.markNull(PlaceId{2});
  EXPECT_TRUE(tracker.isNull(PlaceId{2}));
  EXPECT_FALSE(tracker.holds(PlaceId{2})) << "a null pointer owns nothing";
  tracker.hold(PlaceId{2}, allocated(5, "free"));
  EXPECT_FALSE(tracker.isNull(PlaceId{2})) << "a store clears the null mark";
  tracker.forgetNull(PlaceId{2});
  tracker.forget(PlaceId{2});
  EXPECT_TRUE(tracker.empty());
}

TEST(ResourceTracker, JoinUnionsRecordsAndIntersectsNulls) {
  ResourceTracker a;
  ResourceTracker b;
  a.hold(PlaceId{0}, allocated(1, "free"));
  b.hold(PlaceId{1}, allocated(2, "fclose"));
  a.markNull(PlaceId{5});
  a.markNull(PlaceId{6});
  b.markNull(PlaceId{6});

  EXPECT_TRUE(a.join(b));
  EXPECT_TRUE(a.holds(PlaceId{0}));
  EXPECT_TRUE(a.holds(PlaceId{1})) << "may hold: union";
  EXPECT_FALSE(a.isNull(PlaceId{5})) << "must be null: intersection";
  EXPECT_TRUE(a.isNull(PlaceId{6}));
  EXPECT_FALSE(a.join(b)) << "a second join changes nothing";
}

TEST(ResourceTracker, JoinKeepsThisSideOrsEscapeAndClearsDisagreeingFamily) {
  ResourceTracker a;
  ResourceTracker b;
  a.hold(PlaceId{0}, allocated(1, "free"));
  b.hold(PlaceId{0}, allocated(7, "free"));
  b.escape(PlaceId{0});
  a.hold(PlaceId{1}, allocated(1, "free"));
  b.hold(PlaceId{1}, allocated(1, "fclose"));

  EXPECT_TRUE(a.join(b));
  const auto same = a.recordOf(PlaceId{0});
  ASSERT_TRUE(same);
  EXPECT_EQ(same->location.line, 1U) << "this side's record is kept";
  EXPECT_TRUE(same->escaped) << "escaped on either side is escaped";
  EXPECT_EQ(same->family, "free");
  const auto differ = a.recordOf(PlaceId{1});
  ASSERT_TRUE(differ);
  EXPECT_EQ(differ->family, "") << "an unknown family is never reported";
}

// RFC 0008, *Invalid releases*: a holder that may point into the middle of
// its resource on some path may not release it.
TEST(ResourceTracker, JoinOrsInterior) {
  ResourceTracker a;
  ResourceTracker b;
  a.hold(PlaceId{0}, allocated(1, "free"));
  ResourceRecord offset = allocated(1, "free");
  offset.interior = true;
  b.hold(PlaceId{0}, offset);
  EXPECT_FALSE(a.recordOf(PlaceId{0})->interior);

  EXPECT_TRUE(a.join(b));
  EXPECT_TRUE(a.recordOf(PlaceId{0})->interior);
  EXPECT_FALSE(a.join(b)) << "fixpoint";

  // The other way round: already interior stays interior.
  EXPECT_FALSE(b.join(a));
  EXPECT_TRUE(b.recordOf(PlaceId{0})->interior);
}

TEST(ResourceTracker, ListsHoldersAndNullsInOrder) {
  ResourceTracker tracker;
  tracker.hold(PlaceId{9}, allocated(1, "free"));
  tracker.hold(PlaceId{2}, allocated(1, "free"));
  tracker.markNull(PlaceId{7});
  tracker.markNull(PlaceId{3});
  EXPECT_EQ(tracker.holders(), (std::vector<PlaceId>{PlaceId{2}, PlaceId{9}}));
  EXPECT_EQ(tracker.nullPlaces(),
            (std::vector<PlaceId>{PlaceId{3}, PlaceId{7}}));
}

TEST(ResourceOrigin, SpellsStably) {
  EXPECT_EQ(toString(ResourceOrigin::Allocated), "allocated");
  EXPECT_EQ(toString(ResourceOrigin::Declared), "declared");
}

// -- Guards (RFC 0009, *Refuting guards in the state*) ------------------------

static PlaceGuard when(PlaceId key, const ValueFact &fact) {
  PlaceGuard guard;
  guard.require(key, fact);
  return guard;
}

TEST(ResourceTracker, GuardedResourceIsClearedWhenRefuted) {
  ResourceTracker tracker;
  // `if (c) p = malloc(8);`: held on the arm where `c` is non-zero.
  ResourceRecord record = allocated(3, "free");
  record.guard = when(PlaceId{1}, ValueFact::nonZero());
  tracker.hold(PlaceId{0}, record);
  EXPECT_TRUE(tracker.holds(PlaceId{0}));

  EXPECT_TRUE(
      tracker.learn(PlaceId{1}, ValueFact::of(Outcome::Positive)).empty());
  EXPECT_EQ(tracker.recordOf(PlaceId{0})->guard,
            when(PlaceId{1}, ValueFact::of(Outcome::Positive)))
      << "narrowed by a compatible fact";
  // `if (!c) return;`: on that edge the place never held the resource, so
  // there is nothing to leak.
  EXPECT_EQ(tracker.learn(PlaceId{1}, ValueFact::of(Outcome::Zero)),
            (std::vector<PlaceId>{PlaceId{0}}));
  EXPECT_FALSE(tracker.holds(PlaceId{0}));
}

TEST(ResourceTracker, JoinAndDropWeakenGuards) {
  ResourceTracker left;
  ResourceRecord mine = allocated(3, "free");
  mine.guard = when(PlaceId{1}, ValueFact::of(Outcome::Positive));
  mine.guard.require(PlaceId{2}, ValueFact::of(Outcome::Null));
  left.hold(PlaceId{0}, mine);

  ResourceTracker right;
  ResourceRecord theirs = allocated(3, "free");
  theirs.guard = when(PlaceId{1}, ValueFact::of(Outcome::Negative));
  right.hold(PlaceId{0}, theirs);

  EXPECT_TRUE(left.join(right));
  EXPECT_EQ(left.recordOf(PlaceId{0})->guard,
            when(PlaceId{1}, ValueFact::nonZero()));

  left.dropGuardsOn(PlaceId{1});
  EXPECT_TRUE(left.recordOf(PlaceId{0})->guard.trivial());
  EXPECT_TRUE(left.holds(PlaceId{0}));
}

// RFC 0010, *Shares*: an increment on a holder adds a share; a release takes
// one; the record goes with the last.
TEST(ResourceTracker, RetainAndReleaseCountShares) {
  ResourceTracker tracker;
  tracker.hold(PlaceId{0}, allocated(3, "free"));
  EXPECT_EQ(tracker.recordOf(PlaceId{0})->shares, 1U);
  EXPECT_TRUE(tracker.recordOf(PlaceId{0})->countField.empty());

  const ResourceRecord after =
      tracker.retain(PlaceId{0}, "struct obj.rc", at(5));
  EXPECT_EQ(after.shares, 2U);
  EXPECT_EQ(after.origin, ResourceOrigin::Allocated) << "origin kept";
  EXPECT_EQ(after.location.line, 3U) << "the allocation stays the location";
  EXPECT_EQ(after.countField, "struct obj.rc");

  EXPECT_EQ(tracker.release(PlaceId{0}), 1U);
  EXPECT_TRUE(tracker.holds(PlaceId{0})) << "one share left";
  EXPECT_EQ(tracker.release(PlaceId{0}), 0U);
  EXPECT_FALSE(tracker.holds(PlaceId{0})) << "the last share ends the record";
  EXPECT_EQ(tracker.release(PlaceId{0}), 0U) << "no record: no-op";
}

// RFC 0010, *Retaining*: an increment on a place without a record makes a
// `Retained` one whose location is the increment.
TEST(ResourceTracker, RetainWithoutARecordIsRetained) {
  ResourceTracker tracker;
  const ResourceRecord record =
      tracker.retain(PlaceId{2}, "struct obj.rc", at(7));
  EXPECT_EQ(record.origin, ResourceOrigin::Retained);
  EXPECT_EQ(record.shares, 1U);
  EXPECT_EQ(record.location.line, 7U);
  EXPECT_EQ(record.countField, "struct obj.rc");
  EXPECT_TRUE(record.family.empty());
  ASSERT_TRUE(tracker.holds(PlaceId{2}));
  EXPECT_EQ(*tracker.recordOf(PlaceId{2}), record);
  EXPECT_EQ(toString(ResourceOrigin::Retained), "retained");
}

TEST(ResourceTracker, UnescapeRetractsAnEscape) {
  ResourceTracker tracker;
  tracker.unescape(PlaceId{1});
  EXPECT_FALSE(tracker.holds(PlaceId{1})) << "no record: no-op";
  tracker.hold(PlaceId{1}, allocated(1, "free"));
  tracker.escape(PlaceId{1});
  ASSERT_TRUE(tracker.isEscaped(PlaceId{1}));
  tracker.unescape(PlaceId{1});
  EXPECT_FALSE(tracker.isEscaped(PlaceId{1}));
  EXPECT_TRUE(tracker.holds(PlaceId{1}));
}

// RFC 0010, *Joins*: the share count joins to the smaller (a share owned on
// one path only may already be gone), the count field clears on
// disagreement, and an allocation outranks a retained share.
TEST(ResourceTracker, JoinTakesFewerSharesAndPrefersAllocated) {
  ResourceTracker left;
  left.hold(PlaceId{0}, allocated(3, "free"));
  left.retain(PlaceId{0}, "struct obj.rc", at(4));
  left.retain(PlaceId{0}, "struct obj.rc", at(5));
  ASSERT_EQ(left.recordOf(PlaceId{0})->shares, 3U);

  ResourceTracker right;
  right.hold(PlaceId{0}, allocated(3, "free"));
  right.retain(PlaceId{0}, "struct obj.users", at(6));
  ASSERT_EQ(right.recordOf(PlaceId{0})->shares, 2U);

  EXPECT_TRUE(left.join(right));
  EXPECT_EQ(left.recordOf(PlaceId{0})->shares, 2U);
  EXPECT_TRUE(left.recordOf(PlaceId{0})->countField.empty())
      << "fields disagree";
  EXPECT_EQ(left.recordOf(PlaceId{0})->family, "free");

  ResourceTracker retained;
  retained.retain(PlaceId{1}, "struct obj.rc", at(8));
  ResourceTracker owned;
  owned.hold(PlaceId{1}, allocated(2, "free"));
  EXPECT_TRUE(retained.join(owned));
  EXPECT_EQ(retained.recordOf(PlaceId{1})->origin, ResourceOrigin::Allocated);
  EXPECT_EQ(retained.recordOf(PlaceId{1})->location.line, 2U);
  EXPECT_EQ(retained.recordOf(PlaceId{1})->family, "free");
  EXPECT_EQ(retained.recordOf(PlaceId{1})->shares, 1U);

  // The same join the other way round lands on the same record.
  ResourceTracker owned2;
  owned2.hold(PlaceId{1}, allocated(2, "free"));
  ResourceTracker retained2;
  retained2.retain(PlaceId{1}, "struct obj.rc", at(8));
  owned2.join(retained2);
  EXPECT_EQ(owned2.recordOf(PlaceId{1})->origin, ResourceOrigin::Allocated);
  EXPECT_EQ(owned2.recordOf(PlaceId{1})->shares, 1U);
}

} // namespace
} // namespace weavec::core
