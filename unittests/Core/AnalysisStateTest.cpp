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
  right.raw.markRaw(Q, RawReason::IntegerCast, at(4));

  left.join(right);
  EXPECT_TRUE(left.moves.isMoved(P));
  EXPECT_TRUE(left.aliases.mayAlias(P, Q));
  EXPECT_TRUE(left.loans.hasLoans(X));
  EXPECT_TRUE(left.raw.isRaw(Q)) << "raw on either path is may-raw";
}

// RFC 0006, *Pending outcomes*.
TEST(AnalysisState, JoinKeepsOnlyAgreedPendingOutcomes) {
  PendingOutcome realloc;
  realloc.consumedBy[Outcome::Null] = {};
  realloc.consumedBy[Outcome::NonNull] = {P};

  AnalysisState left;
  left.pending[Q] = realloc;
  AnalysisState right;

  AnalysisState joined = left;
  joined.join(right);
  EXPECT_TRUE(joined.pending.empty())
      << "an outcome pending on one path only cannot be undone";

  right.pending[Q] = realloc;
  joined = left;
  joined.join(right);
  EXPECT_EQ(joined.pending.at(Q), realloc);

  right.pending[Q].consumedBy[Outcome::NonNull] = {X};
  joined = left;
  joined.join(right);
  EXPECT_TRUE(joined.pending.empty()) << "the sides must agree exactly";
}

TEST(AnalysisState, JoinUnionsConsumed) {
  const SummaryPath p = SummaryPath::param(0);
  AnalysisState left;
  left.consumed[p] = PlaceEffect{.freed = true};
  AnalysisState right;
  right.consumed[SummaryPath::param(1)] = PlaceEffect{.moved = true};
  left.join(right);
  EXPECT_EQ(left.consumed.size(), 2U);
  EXPECT_TRUE(left.consumed.at(p).freed);
}

TEST(PendingOutcome, SelectReinstatesWhatTheSelectedClassesKeep) {
  PendingOutcome pending;
  pending.consumedBy[Outcome::Null] = {};
  pending.consumedBy[Outcome::NonNull] = {P, Q};
  EXPECT_EQ(pending.places(), (std::vector<PlaceId>{P, Q}));
  EXPECT_FALSE(pending.settled());

  PendingOutcome onNull = pending;
  EXPECT_EQ(onNull.select({Outcome::Null}), (std::vector<PlaceId>{P, Q}))
      << "realloc failed: the argument is still owned";
  EXPECT_TRUE(onNull.settled());

  PendingOutcome onNonNull = pending;
  EXPECT_TRUE(onNonNull.select({Outcome::NonNull}).empty());
  EXPECT_TRUE(onNonNull.settled()) << "one class left, everything consumed";

  PendingOutcome infeasible = pending;
  EXPECT_TRUE(infeasible.select({Outcome::Zero}).empty())
      << "a class the callee never returns changes nothing";
  EXPECT_EQ(infeasible, pending);

  PendingOutcome status;
  status.consumedBy[Outcome::Zero] = {P};
  status.consumedBy[Outcome::Positive] = {P, Q};
  status.consumedBy[Outcome::Negative] = {};
  PendingOutcome nonNegative = status;
  EXPECT_TRUE(nonNegative.select({Outcome::Zero, Outcome::Positive}).empty())
      << "p is consumed in both selected classes; q only in one";
  EXPECT_FALSE(nonNegative.settled()) << "q can still be retracted by rc == 0";
  EXPECT_EQ(nonNegative.select({Outcome::Zero}), std::vector<PlaceId>{Q});
  EXPECT_TRUE(nonNegative.settled());
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
  EXPECT_FALSE(b.join(a)) << "joining a state with itself changes nothing";
  EXPECT_EQ(a, b);
}

TEST(AnalysisState, JoinReportsEveryKindOfChange) {
  const auto changes = [](auto mutate) {
    AnalysisState base;
    base.kinds[P] = OwnershipKind::Owned;
    base.consumed[SummaryPath::param(0)] = PlaceEffect{.freed = true};
    base.pending[Q] = PendingOutcome{.consumedBy = {{Outcome::Null, {P}}}};
    AnalysisState other = base;
    mutate(other);
    AnalysisState joined = base;
    const bool changed = joined.join(other);
    EXPECT_EQ(changed, joined != base);
    return changed;
  };
  EXPECT_TRUE(changes([](AnalysisState &s) {
    s.moves.markMoved(P, MoveReason::Freed, at(1));
  }));
  EXPECT_TRUE(changes([](AnalysisState &s) { s.aliases.unite(P, Q); }));
  EXPECT_TRUE(changes(
      [](AnalysisState &s) { s.loans.addLoanUnchecked(loanOn(X, Q)); }));
  EXPECT_TRUE(changes([](AnalysisState &s) {
    s.raw.markRaw(Q, RawReason::IntegerCast, at(2));
  }));
  EXPECT_TRUE(changes([](AnalysisState &s) { s.pending.erase(Q); }))
      << "a pending outcome on one side only is dropped";
  EXPECT_TRUE(changes([](AnalysisState &s) {
    s.consumed[SummaryPath::param(0)].moved = true;
  }));
  EXPECT_TRUE(
      changes([](AnalysisState &s) { s.kinds[P] = OwnershipKind::Shared; }));
  EXPECT_FALSE(changes([](AnalysisState &) {}));
}

TEST(AnalysisState, ForgetClearsEveryFactAboutThePlace) {
  AnalysisState state;
  state.moves.markMoved(P, MoveReason::Freed, at(3));
  state.aliases.unite(P, Q);
  state.loans.addLoanUnchecked(loanOn(X, P));
  state.loans.addLoanUnchecked(loanOn(P, Q));
  state.pending[P].consumedBy[Outcome::NonNull] = {Q};
  state.kinds[P] = OwnershipKind::Owned;
  state.raw.markRaw(P, RawReason::Declared, at(3));

  state.forget(P);
  EXPECT_FALSE(state.raw.isRaw(P));
  EXPECT_FALSE(state.moves.isMoved(P));
  EXPECT_FALSE(state.aliases.mayAlias(P, Q));
  EXPECT_TRUE(state.loans.heldBy(P).empty()) << "loans held by p dropped";
  EXPECT_FALSE(state.loans.hasLoans(P)) << "loans against p dropped";
  EXPECT_TRUE(state.pending.empty());
  EXPECT_EQ(state.kindOf(P), OwnershipKind::Unknown);
}

} // namespace
} // namespace weavec::core
