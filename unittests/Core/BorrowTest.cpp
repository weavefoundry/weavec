//===- BorrowTest.cpp - Tests for loan tracking ---------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Borrow.h"

#include <gtest/gtest.h>

namespace weavec::core {

static Loan makeLoan(PlaceId place, BorrowKind kind, LifetimeId lifetime) {
  return Loan{
      .place = place, .kind = kind, .lifetime = lifetime, .location = {}};
}

namespace {

class BorrowStateTest : public ::testing::Test {
protected:
  PlaceTable places;
  PlaceId p = places.create("p");
  PlaceId q = places.create("q");
  LifetimeId l1{1};
  LifetimeId l2{2};
  BorrowState state;
};

TEST_F(BorrowStateTest, SharedBorrowsCoexist) {
  EXPECT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  EXPECT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l2)));
  EXPECT_EQ(state.loans().size(), 2U);
}

TEST_F(BorrowStateTest, MutableExcludesShared) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Mutable, l1)));
  const auto conflict = state.addLoan(makeLoan(p, BorrowKind::Shared, l2));
  ASSERT_TRUE(conflict);
  EXPECT_EQ(conflict->existing.kind, BorrowKind::Mutable);
  ASSERT_TRUE(conflict->attempted);
  EXPECT_EQ(conflict->attempted->kind, BorrowKind::Shared);
  EXPECT_EQ(state.loans().size(), 1U) << "rejected loans are not recorded";
}

TEST_F(BorrowStateTest, SharedExcludesMutable) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  EXPECT_TRUE(state.addLoan(makeLoan(p, BorrowKind::Mutable, l2)));
}

TEST_F(BorrowStateTest, MutableExcludesMutable) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Mutable, l1)));
  EXPECT_TRUE(state.addLoan(makeLoan(p, BorrowKind::Mutable, l2)));
}

TEST_F(BorrowStateTest, DistinctPlacesNeverConflict) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Mutable, l1)));
  EXPECT_FALSE(state.addLoan(makeLoan(q, BorrowKind::Mutable, l1)));
}

TEST_F(BorrowStateTest, MoveBlockedByAnyLoan) {
  EXPECT_FALSE(state.checkMove(p));
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  const auto conflict = state.checkMove(p);
  ASSERT_TRUE(conflict);
  EXPECT_FALSE(conflict->attempted);
  EXPECT_FALSE(state.checkMove(q));
}

TEST_F(BorrowStateTest, MutationBlockedByAnyLoan) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  EXPECT_TRUE(state.checkMutation(p));
  EXPECT_FALSE(state.checkMutation(q));
}

TEST_F(BorrowStateTest, ExpireDropsLoansForLifetime) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  ASSERT_FALSE(state.addLoan(makeLoan(q, BorrowKind::Shared, l2)));
  state.expire(l1);
  EXPECT_FALSE(state.hasLoans(p));
  EXPECT_TRUE(state.hasLoans(q));
  EXPECT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Mutable, l2)));
}

TEST_F(BorrowStateTest, ReleaseDropsLoansForPlace) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l2)));
  ASSERT_FALSE(state.addLoan(makeLoan(q, BorrowKind::Shared, l1)));
  state.release(p);
  EXPECT_FALSE(state.hasLoans(p));
  EXPECT_TRUE(state.hasLoans(q));
}

} // namespace
} // namespace weavec::core
