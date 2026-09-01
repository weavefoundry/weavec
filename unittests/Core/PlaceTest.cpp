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

TEST(PlaceId, Ordered) {
  EXPECT_LT(PlaceId{1}, PlaceId{2});
  EXPECT_GE(PlaceId{2}, PlaceId{2});
}

class PlacePathTest : public ::testing::Test {
protected:
  PlaceTable table;
  PlaceId p = table.create("p");
  PlaceId s = table.create("s");
  PlaceId a = table.create("a");
};

TEST_F(PlacePathTest, FieldsAreInternedAndNamed) {
  const PlaceId sf = table.field(s, "f");
  EXPECT_EQ(table.field(s, "f"), sf) << "same path, same id";
  EXPECT_NE(table.field(s, "g"), sf);
  EXPECT_EQ(table.name(sf), "s.f");
  EXPECT_EQ(table.parent(sf), s);
  EXPECT_EQ(table.step(sf), PathStep::Field);
  EXPECT_EQ(table.fieldName(sf), "f");
  EXPECT_FALSE(table.isBase(sf));
  EXPECT_TRUE(table.isBase(s));
}

TEST_F(PlacePathTest, ArrowIsFieldOfDeref) {
  const PlaceId star = table.deref(p);
  const PlaceId next = table.field(star, "next");
  EXPECT_EQ(table.name(star), "*p");
  EXPECT_EQ(table.name(next), "p->next");
  EXPECT_EQ(table.name(table.deref(next)), "*p->next");
  EXPECT_EQ(table.name(table.field(table.deref(next), "v")), "p->next->v");
  EXPECT_EQ(table.root(next), p);
  EXPECT_EQ(table.depth(next), 2U);
  EXPECT_EQ(table.depth(p), 0U);
}

TEST_F(PlacePathTest, IndexCollapses) {
  const PlaceId elems = table.index(a);
  EXPECT_EQ(table.name(elems), "a[*]");
  EXPECT_EQ(table.index(elems), elems) << "a[*][*] is a[*]";
  const PlaceId star = table.deref(p);
  EXPECT_EQ(table.index(star), star) << "(*p)[*] is *p";
  EXPECT_EQ(table.name(table.field(elems, "f")), "a[*].f");
}

TEST_F(PlacePathTest, AncestorsAndDescendants) {
  const PlaceId star = table.deref(p);
  const PlaceId next = table.field(star, "next");
  const PlaceId data = table.field(star, "data");

  EXPECT_TRUE(table.isDescendantOf(next, p));
  EXPECT_TRUE(table.isDescendantOf(next, star));
  EXPECT_FALSE(table.isDescendantOf(p, next));
  EXPECT_FALSE(table.isDescendantOf(p, p));

  EXPECT_EQ(table.descendants(p), (std::vector<PlaceId>{star, next, data}));
  EXPECT_EQ(table.ancestors(next), (std::vector<PlaceId>{star, p}));
  EXPECT_TRUE(table.descendants(next).empty());
}

TEST_F(PlacePathTest, TranslateRebuildsPathUnderNewPrefix) {
  const PlaceId q = table.create("q");
  const PlaceId pNext = table.field(table.deref(p), "next");
  const PlaceId pNextBuf = table.index(table.field(table.deref(pNext), "buf"));

  const PlaceId qNextBuf = table.translate(pNextBuf, p, q);
  EXPECT_EQ(table.name(qNextBuf), "q->next->buf[*]");
  EXPECT_EQ(table.translate(pNextBuf, table.deref(p), table.deref(q)),
            qNextBuf);
  EXPECT_EQ(table.translate(p, p, q), q);
}

TEST_F(PlacePathTest, InnermostDeref) {
  const PlaceId star = table.deref(p);
  const PlaceId next = table.field(star, "next");
  const PlaceId nextV = table.field(table.deref(next), "v");
  EXPECT_EQ(table.innermostDeref(nextV), table.deref(next));
  EXPECT_EQ(table.innermostDeref(next), star);
  EXPECT_EQ(table.innermostDeref(star), star);
  EXPECT_FALSE(table.innermostDeref(table.field(s, "f")));
  EXPECT_FALSE(table.innermostDeref(p));
}

} // namespace
} // namespace weavec::core
