//===- PlaceTest.cpp - Tests for the place table --------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Place.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace weavec::core {
namespace {

TEST(PlaceTable, AssignsSequentialIds) {
  PlaceTable table;
  const PlaceId a = table.create("a");
  const PlaceId b = table.create("b");
  EXPECT_NE(a, b);
  EXPECT_EQ(a.value, 0U);
  EXPECT_EQ(b.value, 1U);
  EXPECT_EQ(table.size(), 2U);
}

TEST(PlaceTable, NamesRoundTrip) {
  PlaceTable table;
  const PlaceId a = table.create("buffer");
  EXPECT_EQ(table.name(a), "buffer");
  EXPECT_EQ(table.name(PlaceId{99}), "<unknown place>");
}

TEST(PlaceId, Hashable) {
  std::unordered_set<PlaceId, PlaceIdHash> set;
  set.insert(PlaceId{1});
  set.insert(PlaceId{1});
  set.insert(PlaceId{2});
  EXPECT_EQ(set.size(), 2U);
}

} // namespace
} // namespace weavec::core
