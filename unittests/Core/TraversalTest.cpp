//===- TraversalTest.cpp - Traversal proof algebra (RFC 0021) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Traversal.h"

#include <gtest/gtest.h>

#include <limits>

namespace weavec::core {

TEST(Traversal, RelationDirectionsMatchConcreteIntegers) {
  const PlaceId x{0};
  const PlaceId y{1};
  for (const auto relation :
       {Relation::Less, Relation::LessEqual, Relation::Equal,
        Relation::GreaterEqual, Relation::Greater})
    for (int offset = -2; offset <= 2; ++offset) {
      DifferenceConstraints facts;
      facts.learn(x, {.relation = relation, .offset = offset}, y);
      for (int a = -4; a <= 4; ++a)
        for (int b = -4; b <= 4; ++b) {
          bool holds = false;
          switch (relation) {
          case Relation::Less:
            holds = a < b + offset;
            break;
          case Relation::LessEqual:
            holds = a <= b + offset;
            break;
          case Relation::Equal:
            holds = a == b + offset;
            break;
          case Relation::GreaterEqual:
            holds = a >= b + offset;
            break;
          case Relation::Greater:
            holds = a > b + offset;
            break;
          }
          if (!holds)
            continue;
          if (const auto bound = facts.bound(x, y))
            EXPECT_LE(a - b, *bound);
          if (const auto bound = facts.bound(y, x))
            EXPECT_LE(b - a, *bound);
        }
    }
}

TEST(Traversal, DifferenceClosurePreservesDirectionAndZeroIdentity) {
  const PlaceId x{0};
  const PlaceId y{1};
  const PlaceId z{2};
  DifferenceConstraints facts;
  facts.constrain(x, y, -1);
  facts.constrain(y, z, 2);
  EXPECT_EQ(facts.bound(x, z), 1);
  EXPECT_FALSE(facts.bound(z, x));
  facts.constrain(z, {}, 8);
  EXPECT_EQ(facts.bound(x, {}), 9);
  EXPECT_FALSE(facts.bound({}, x));
}

TEST(Traversal, HostOverflowAndVariableExhaustionLoseProof) {
  DifferenceConstraints facts;
  facts.constrain(PlaceId{0}, PlaceId{1}, INT64_MIN);
  facts.constrain(PlaceId{1}, PlaceId{2}, -1);
  EXPECT_FALSE(facts.bound(PlaceId{0}, PlaceId{2}));
  for (unsigned i = 0; i <= MaxTraversalVariables; ++i)
    facts.constrain(PlaceId{i}, {}, 3);
  EXPECT_TRUE(facts.limited());
  DifferenceConstraints full;
  for (unsigned i = 0; i < MaxTraversalVariables; i += 2)
    EXPECT_TRUE(full.constrain(PlaceId{i}, PlaceId{i + 1}, 3));
  EXPECT_FALSE(full.limited());
  // An endpoint seen only on the right is still an existing variable.
  EXPECT_TRUE(full.constrain(PlaceId{63}, PlaceId{0}, 3));
  EXPECT_TRUE(full.constrain(PlaceId{62}, PlaceId{60}, 3));
  EXPECT_TRUE(full.constrain({}, PlaceId{63}, 3));
  EXPECT_FALSE(full.limited());
  EXPECT_FALSE(full.constrain(PlaceId{64}, {}, 3));
  EXPECT_TRUE(full.limited());
}

TEST(Traversal, ClosureWorkExhaustionIsReportedAndSuppliesNoProof) {
  DifferenceConstraints facts;
  for (unsigned i = 0; i + 1 < MaxTraversalVariables; ++i) {
    facts.constrain(PlaceId{i}, PlaceId{i + 1}, 0);
    if (i + 2 < MaxTraversalVariables)
      facts.constrain(PlaceId{i}, PlaceId{i + 2}, 0);
  }
  EXPECT_FALSE(facts.limited());
  EXPECT_FALSE(facts.bound(PlaceId{0}, PlaceId{63}));
  EXPECT_TRUE(facts.limited());
}

} // namespace weavec::core
