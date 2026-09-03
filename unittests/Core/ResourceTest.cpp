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

} // namespace
} // namespace weavec::core
