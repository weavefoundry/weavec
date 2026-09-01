//===- LifetimeTest.cpp - Tests for lifetime constraints ------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Lifetime.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

TEST(LifetimeConstraints, StaticOutlivesEverything) {
  LifetimeConstraints constraints;
  const LifetimeId a = constraints.fresh("'a");
  const LifetimeId b = constraints.fresh("'b");

  EXPECT_TRUE(constraints.outlives(LifetimeId::staticLifetime(), a));
  EXPECT_TRUE(constraints.outlives(LifetimeId::staticLifetime(), b));
  EXPECT_FALSE(constraints.outlives(a, LifetimeId::staticLifetime()));
}

TEST(LifetimeConstraints, Reflexive) {
  LifetimeConstraints constraints;
  const LifetimeId a = constraints.fresh();
  EXPECT_TRUE(constraints.outlives(a, a));
}

TEST(LifetimeConstraints, UnrelatedLifetimesDoNotOutlive) {
  LifetimeConstraints constraints;
  const LifetimeId a = constraints.fresh();
  const LifetimeId b = constraints.fresh();
  EXPECT_FALSE(constraints.outlives(a, b));
  EXPECT_FALSE(constraints.outlives(b, a));
}

TEST(LifetimeConstraints, DirectConstraint) {
  LifetimeConstraints constraints;
  const LifetimeId a = constraints.fresh();
  const LifetimeId b = constraints.fresh();
  constraints.addOutlives(a, b);
  EXPECT_TRUE(constraints.outlives(a, b));
  EXPECT_FALSE(constraints.outlives(b, a));
}

TEST(LifetimeConstraints, Transitive) {
  LifetimeConstraints constraints;
  const LifetimeId a = constraints.fresh();
  const LifetimeId b = constraints.fresh();
  const LifetimeId c = constraints.fresh();
  constraints.addOutlives(a, b);
  constraints.addOutlives(b, c);
  EXPECT_TRUE(constraints.outlives(a, c));
  EXPECT_FALSE(constraints.outlives(c, a));
}

TEST(LifetimeConstraints, CyclesTerminate) {
  LifetimeConstraints constraints;
  const LifetimeId a = constraints.fresh();
  const LifetimeId b = constraints.fresh();
  constraints.addOutlives(a, b);
  constraints.addOutlives(b, a);
  EXPECT_TRUE(constraints.outlives(a, b));
  EXPECT_TRUE(constraints.outlives(b, a));
}

TEST(LifetimeConstraints, Names) {
  LifetimeConstraints constraints;
  const LifetimeId named = constraints.fresh("'fn");
  const LifetimeId anonymous = constraints.fresh();
  EXPECT_EQ(constraints.name(LifetimeId::staticLifetime()), "'static");
  EXPECT_EQ(constraints.name(named), "'fn");
  EXPECT_EQ(constraints.name(anonymous), "'2");
  EXPECT_EQ(constraints.size(), 3U);
}

} // namespace
} // namespace weavec::core
