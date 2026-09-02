//===- RawTest.cpp - Tests for raw pointer tracking -----------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Raw.h"

#include <gtest/gtest.h>

namespace weavec::core {

static SourceLocation at(std::uint32_t line) {
  return SourceLocation{.file = "t.c", .line = line, .column = 1, .opaque = 0};
}

namespace {

TEST(RawTracker, FreshPlaceIsNotRaw) {
  RawTracker tracker;
  EXPECT_FALSE(tracker.isRaw(PlaceId{0}));
  EXPECT_FALSE(tracker.rawAt(PlaceId{0}));
  EXPECT_TRUE(tracker.empty());
}

TEST(RawTracker, MarkRawRecordsReasonAndLocation) {
  RawTracker tracker;
  EXPECT_TRUE(tracker.markRaw(PlaceId{0}, RawReason::IntegerCast, at(10)));
  const auto record = tracker.rawAt(PlaceId{0});
  ASSERT_TRUE(record);
  EXPECT_EQ(record->reason, RawReason::IntegerCast);
  EXPECT_EQ(record->location.line, 10U);
  EXPECT_FALSE(record->via);
}

TEST(RawTracker, FirstRecordWins) {
  RawTracker tracker;
  ASSERT_TRUE(tracker.markRaw(PlaceId{0}, RawReason::Declared, at(1)));
  EXPECT_FALSE(tracker.markRaw(PlaceId{0}, RawReason::IntegerCast, at(2)));
  EXPECT_EQ(tracker.rawAt(PlaceId{0})->reason, RawReason::Declared);
  EXPECT_EQ(tracker.rawAt(PlaceId{0})->location.line, 1U);
}

TEST(RawTracker, ClearForgets) {
  RawTracker tracker;
  ASSERT_TRUE(tracker.markRaw(PlaceId{0}, RawReason::Callee, at(1)));
  tracker.clear(PlaceId{0});
  EXPECT_FALSE(tracker.isRaw(PlaceId{0}));
  EXPECT_TRUE(tracker.markRaw(PlaceId{0}, RawReason::Callee, at(2)));
}

TEST(RawTracker, CopyPropagatesRecordWithVia) {
  RawTracker tracker;
  ASSERT_TRUE(tracker.markRaw(PlaceId{0}, RawReason::UnknownCallee, at(1)));
  RawRecord copied = *tracker.rawAt(PlaceId{0});
  copied.via = PlaceId{0};
  ASSERT_TRUE(tracker.markRaw(PlaceId{1}, copied));
  EXPECT_EQ(tracker.rawAt(PlaceId{1})->reason, RawReason::UnknownCallee);
  EXPECT_EQ(tracker.rawAt(PlaceId{1})->via, PlaceId{0});
}

TEST(RawTracker, JoinIsUnion) {
  RawTracker a;
  RawTracker b;
  ASSERT_TRUE(a.markRaw(PlaceId{0}, RawReason::IntegerCast, at(1)));
  ASSERT_TRUE(b.markRaw(PlaceId{1}, RawReason::Declared, at(2)));
  ASSERT_TRUE(b.markRaw(PlaceId{0}, RawReason::Declared, at(3)));

  a.join(b);
  EXPECT_TRUE(a.isRaw(PlaceId{0}));
  EXPECT_TRUE(a.isRaw(PlaceId{1}));
  EXPECT_EQ(a.rawAt(PlaceId{0})->location.line, 1U)
      << "existing records are preserved on join";
  EXPECT_FALSE(a.isRaw(PlaceId{2}));
}

TEST(RawTracker, EqualityAndOrderedListing) {
  RawTracker a;
  RawTracker b;
  EXPECT_EQ(a, b);
  ASSERT_TRUE(a.markRaw(PlaceId{2}, RawReason::IntegerCast, at(1)));
  ASSERT_TRUE(a.markRaw(PlaceId{0}, RawReason::IntegerCast, at(1)));
  EXPECT_NE(a, b);
  ASSERT_TRUE(b.markRaw(PlaceId{0}, RawReason::IntegerCast, at(1)));
  ASSERT_TRUE(b.markRaw(PlaceId{2}, RawReason::IntegerCast, at(1)));
  EXPECT_EQ(a, b) << "insertion order is irrelevant";
  EXPECT_EQ(a.rawPlaces(), (std::vector<PlaceId>{PlaceId{0}, PlaceId{2}}));
}

TEST(RawReason, StableSpellings) {
  EXPECT_EQ(toString(RawReason::IntegerCast), "integer-cast");
  EXPECT_EQ(toString(RawReason::Declared), "declared");
  EXPECT_EQ(toString(RawReason::LoadedThroughRaw), "loaded-through-raw");
  EXPECT_EQ(toString(RawReason::Callee), "callee");
  EXPECT_EQ(toString(RawReason::UnknownCallee), "unknown-callee");
}

} // namespace
} // namespace weavec::core
