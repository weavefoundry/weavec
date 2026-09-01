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

} // namespace
} // namespace weavec::core
