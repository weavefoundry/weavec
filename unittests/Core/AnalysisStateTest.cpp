//===- AnalysisStateTest.cpp - Tests for the dataflow state ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AnalysisState.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

constexpr PlaceId P{0};
constexpr PlaceId Q{1};
constexpr PlaceId X{2};

SourceLocation at(std::uint32_t line) {
  return SourceLocation{.file = "t.c", .line = line, .column = 1, .opaque = 0};
}

Loan loanOn(PlaceId place, PlaceId holder,
            BorrowKind kind = BorrowKind::Shared) {
  return Loan{.place = place,
              .kind = kind,
              .lifetime = LifetimeId{1},
              .location = at(1),
              .holder = holder};
}

TEST(AnalysisState, FreshStatesAreEqual) {
  EXPECT_EQ(AnalysisState{}, AnalysisState{});
}

TEST(AnalysisState, JoinUnionsMovesLoansAndAliases) {
  AnalysisState left;
  left.moves.markMoved(P, MoveReason::Freed, at(3));
  left.aliases.unite(P, Q);

  AnalysisState right;
  right.loans.addLoanUnchecked(loanOn(X, Q));

  left.join(right);
  EXPECT_TRUE(left.moves.isMoved(P));
  EXPECT_TRUE(left.aliases.mayAlias(P, Q));
  EXPECT_TRUE(left.loans.hasLoans(X));
}

TEST(AnalysisState, JoinKeepsOnlyAgreedReallocs) {
  AnalysisState left;
  left.reallocs[Q] = {P};
  AnalysisState right;

  AnalysisState joined = left;
  joined.join(right);
  EXPECT_TRUE(joined.reallocs.empty())
      << "a realloc pending on one path only cannot be undone";

  right.reallocs[Q] = {P};
  joined = left;
  joined.join(right);
  EXPECT_EQ(joined.reallocs.at(Q), std::vector<PlaceId>{P});
}

TEST(AnalysisState, JoinLiftsKindsThroughTheLattice) {
  AnalysisState left;
  left.kinds[P] = OwnershipKind::Owned;
  AnalysisState right;
  right.kinds[P] = OwnershipKind::Shared;
  right.kinds[Q] = OwnershipKind::Owned;

  left.join(right);
  EXPECT_EQ(left.kindOf(P), OwnershipKind::Raw) << "conflicting kinds";
  EXPECT_EQ(left.kindOf(Q), OwnershipKind::Owned) << "unknown joins to known";
  EXPECT_EQ(left.kindOf(X), OwnershipKind::Unknown);
}

TEST(AnalysisState, JoinReachesFixpoint) {
  AnalysisState a;
  a.moves.markMoved(P, MoveReason::Freed, at(3));
  a.aliases.unite(P, Q);
  a.loans.addLoanUnchecked(loanOn(X, Q));
  a.kinds[P] = OwnershipKind::Owned;

  AnalysisState b = a;
  b.join(a);
  EXPECT_EQ(a, b) << "joining a state with itself changes nothing";
}

TEST(AnalysisState, ForgetClearsEveryFactAboutThePlace) {
  AnalysisState state;
  state.moves.markMoved(P, MoveReason::Freed, at(3));
  state.aliases.unite(P, Q);
  state.loans.addLoanUnchecked(loanOn(X, P));
  state.loans.addLoanUnchecked(loanOn(P, Q));
  state.reallocs[P] = {Q};
  state.kinds[P] = OwnershipKind::Owned;

  state.forget(P);
  EXPECT_FALSE(state.moves.isMoved(P));
  EXPECT_FALSE(state.aliases.mayAlias(P, Q));
  EXPECT_TRUE(state.loans.heldBy(P).empty()) << "loans held by p dropped";
  EXPECT_FALSE(state.loans.hasLoans(P)) << "loans against p dropped";
  EXPECT_TRUE(state.reallocs.empty());
  EXPECT_EQ(state.kindOf(P), OwnershipKind::Unknown);
}

} // namespace
} // namespace weavec::core
