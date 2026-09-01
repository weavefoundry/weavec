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
  return Loan{.place = place,
              .kind = kind,
              .lifetime = lifetime,
              .location = {},
              .holder = PlaceId{100}};
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

TEST_F(BorrowStateTest, FindConflictDoesNotRecord) {
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Mutable, l1)));
  EXPECT_TRUE(state.findConflict(makeLoan(p, BorrowKind::Shared, l2)));
  EXPECT_FALSE(state.findConflict(makeLoan(q, BorrowKind::Mutable, l2)));
  EXPECT_EQ(state.loans().size(), 1U);
}

TEST_F(BorrowStateTest, HoldersCanBeCopiedAndDropped) {
  const PlaceId a = places.create("a");
  const PlaceId b = places.create("b");
  Loan loan = makeLoan(p, BorrowKind::Mutable, l1);
  loan.holder = a;
  ASSERT_FALSE(state.addLoan(loan));

  // `b = a` shares the borrow rather than creating a second one.
  state.copyHolder(a, b);
  EXPECT_EQ(state.heldBy(b).size(), 1U);
  EXPECT_EQ(state.heldBy(b)[0].place, p);
  EXPECT_EQ(state.loans().size(), 2U);

  state.dropHolder(a);
  EXPECT_TRUE(state.heldBy(a).empty());
  EXPECT_TRUE(state.hasLoans(p)) << "b still holds it";
  state.dropHolder(b);
  EXPECT_FALSE(state.hasLoans(p));
}

TEST_F(BorrowStateTest, AddLoanUncheckedIgnoresConflictsAndDuplicates) {
  const Loan loan = makeLoan(p, BorrowKind::Mutable, l1);
  state.addLoanUnchecked(loan);
  state.addLoanUnchecked(loan);
  state.addLoanUnchecked(makeLoan(p, BorrowKind::Mutable, l2));
  EXPECT_EQ(state.loans().size(), 2U);
}

TEST_F(BorrowStateTest, JoinIsSetUnionAndEqualityIgnoresOrder) {
  BorrowState other;
  ASSERT_FALSE(state.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  ASSERT_FALSE(state.addLoan(makeLoan(q, BorrowKind::Shared, l1)));
  ASSERT_FALSE(other.addLoan(makeLoan(q, BorrowKind::Shared, l1)));
  ASSERT_FALSE(other.addLoan(makeLoan(p, BorrowKind::Shared, l1)));
  EXPECT_EQ(state, other);

  ASSERT_FALSE(other.addLoan(makeLoan(q, BorrowKind::Shared, l2)));
  EXPECT_NE(state, other);
  state.join(other);
  EXPECT_EQ(state, other);
  EXPECT_EQ(state.loans().size(), 3U);
}

TEST(BorrowKind, ToString) {
  EXPECT_EQ(toString(BorrowKind::Shared), "shared");
  EXPECT_EQ(toString(BorrowKind::Mutable), "mutable");
}

} // namespace
} // namespace weavec::core
