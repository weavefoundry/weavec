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

// RFC 0008, *Per-outcome non-null facts*: `int make(T **out)` that stores a
// fresh object and returns 1, or leaves `*out` null and returns 0.
TEST(PendingOutcome, NonNullInAllFollowsTheSelectedClasses) {
  PendingOutcome pending;
  pending.consumedBy[Outcome::Zero] = {};
  pending.consumedBy[Outcome::Positive] = {};
  pending.nullOn[Outcome::Zero] = {P};
  pending.nonNullOn[Outcome::Positive] = {P};
  EXPECT_TRUE(pending.nullInAll().empty()) << "both classes still possible";
  EXPECT_TRUE(pending.nonNullInAll().empty());

  PendingOutcome success = pending;
  success.select({Outcome::Positive});
  EXPECT_EQ(success.nonNullInAll(), std::vector<PlaceId>{P});
  EXPECT_TRUE(success.nullInAll().empty());

  PendingOutcome failure = pending;
  failure.select({Outcome::Zero});
  EXPECT_EQ(failure.nullInAll(), std::vector<PlaceId>{P});
  EXPECT_TRUE(failure.nonNullInAll().empty());

  // Non-null in every class (an out-parameter always set): known without a
  // test.
  PendingOutcome always;
  always.consumedBy[Outcome::Zero] = {};
  always.consumedBy[Outcome::Negative] = {};
  always.nonNullOn[Outcome::Zero] = {P, Q};
  always.nonNullOn[Outcome::Negative] = {P};
  EXPECT_EQ(always.nonNullInAll(), std::vector<PlaceId>{P});
  EXPECT_TRUE(always.places().empty()) << "nothing consumed";
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
    base.overwritten.insert(SummaryPath::param(1).deref());
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
  EXPECT_TRUE(changes([](AnalysisState &s) {
    s.nulls.set(P, NullRecord{.state = Nullness::Null,
                              .location = at(2),
                              .reason = NullReason::AssignedNull,
                              .detail = {}});
  }));
  EXPECT_TRUE(changes([](AnalysisState &s) { s.overwritten.clear(); }))
      << "overwritten on one side only is not overwritten";
  EXPECT_FALSE(changes([](AnalysisState &s) {
    s.overwritten.insert(SummaryPath::param(0).deref());
  })) << "a path only the other side overwrote adds nothing";
  EXPECT_FALSE(changes([](AnalysisState &) {}));
}

// RFC 0008, *Nullness*: the null component joins per place.
TEST(AnalysisState, JoinFoldsNullness) {
  AnalysisState left;
  left.nulls.set(P, NullRecord{.state = Nullness::NonNull,
                               .location = at(1),
                               .reason = NullReason::Tested,
                               .detail = {}});
  AnalysisState right;
  right.nulls.set(P, NullRecord{.state = Nullness::Null,
                                .location = at(2),
                                .reason = NullReason::AssignedNull,
                                .detail = {}});
  EXPECT_TRUE(left.join(right));
  EXPECT_EQ(left.nulls.stateOf(P), Nullness::MaybeNull);
  EXPECT_EQ(left.nulls.recordOf(P)->location.line, 2U);
}

// RFC 0008, *Replaced values*: a caller-visible path counts as overwritten
// only when every path reaching the point replaced it.
TEST(AnalysisState, OverwrittenIsAMustFact) {
  const SummaryPath items = SummaryPath::param(0).deref().field("items");
  AnalysisState left;
  left.overwritten.insert(items);
  left.overwritten.insert(SummaryPath::param(1));
  AnalysisState right;
  right.overwritten.insert(items);

  EXPECT_TRUE(left.isOverwritten(items));
  EXPECT_TRUE(left.isOverwritten(SummaryPath::param(1)));
  EXPECT_TRUE(left.join(right));
  EXPECT_TRUE(left.isOverwritten(items));
  EXPECT_FALSE(left.isOverwritten(SummaryPath::param(1)));

  // Overwriting the struct overwrites its fields, but not what the fields
  // pointed to: no dereference step may separate the prefix from the path.
  AnalysisState whole;
  whole.overwritten.insert(SummaryPath::param(0).deref());
  EXPECT_TRUE(whole.isOverwritten(SummaryPath::param(0).deref()));
  EXPECT_TRUE(whole.isOverwritten(items));
  EXPECT_FALSE(whole.isOverwritten(items.deref()));
  EXPECT_FALSE(whole.isOverwritten(SummaryPath::param(0)));
  EXPECT_FALSE(whole.isOverwritten(SummaryPath::param(1).deref()));
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
  state.nulls.set(P, NullRecord{.state = Nullness::Null,
                                .location = at(3),
                                .reason = NullReason::AssignedNull,
                                .detail = {}});

  state.scalars.set(P, ValueFact::ofConstant(0));
  PlaceGuard aboutP;
  aboutP.require(P, ValueFact::of(Outcome::Null));
  state.moves.markMoved(Q, MoveReason::Freed, at(4), {},
                        ElementWitness::whole(), "free", false, aboutP);

  state.forget(P);
  EXPECT_FALSE(state.nulls.stateOf(P));
  EXPECT_FALSE(state.raw.isRaw(P));
  EXPECT_FALSE(state.moves.isMoved(P));
  EXPECT_FALSE(state.aliases.mayAlias(P, Q));
  EXPECT_TRUE(state.loans.heldBy(P).empty()) << "loans held by p dropped";
  EXPECT_FALSE(state.loans.hasLoans(P)) << "loans against p dropped";
  EXPECT_TRUE(state.pending.empty());
  EXPECT_EQ(state.kindOf(P), OwnershipKind::Unknown);
  EXPECT_FALSE(state.scalars.factOf(P)) << "scalar fact dropped (RFC 0009)";
  EXPECT_TRUE(state.moves.recordOf(Q)->guard.trivial())
      << "guards about p dropped, the record kept";
}

// -- RFC 0009: scalar facts and guards ----------------------------------------

TEST(AnalysisState, FactOfReadsScalarsThenDefiniteNullness) {
  AnalysisState state;
  EXPECT_FALSE(state.factOf(P));
  state.scalars.set(P, ValueFact::ofConstant(3));
  EXPECT_EQ(state.factOf(P), ValueFact::ofConstant(3));

  state.nulls.set(Q, NullRecord{.state = Nullness::Null,
                                .location = at(1),
                                .reason = NullReason::AssignedNull,
                                .detail = {}});
  EXPECT_EQ(state.factOf(Q), ValueFact::of(Outcome::Null));
  state.nulls.set(Q, NullRecord{.state = Nullness::NonNull,
                                .location = at(1),
                                .reason = NullReason::Dereferenced,
                                .detail = {}});
  EXPECT_EQ(state.factOf(Q), ValueFact::of(Outcome::NonNull));
  state.nulls.set(Q, NullRecord{.state = Nullness::MaybeNull,
                                .location = at(1),
                                .reason = NullReason::CalleeResult,
                                .detail = {}});
  EXPECT_FALSE(state.factOf(Q)) << "maybe-null is no fact";
}

TEST(AnalysisState, PathGuardIsTheScalarFactsAndTestedNullness) {
  AnalysisState state;
  EXPECT_TRUE(state.pathGuard().trivial());
  state.scalars.set(P, ValueFact::nonZero());
  state.nulls.set(Q, NullRecord{.state = Nullness::Null,
                                .location = at(1),
                                .reason = NullReason::AssignedNull,
                                .detail = {}});
  state.nulls.set(X, NullRecord{.state = Nullness::NonNull,
                                .location = at(1),
                                .reason = NullReason::Dereferenced,
                                .detail = {}});
  PlaceGuard expected;
  expected.require(P, ValueFact::nonZero());
  expected.require(Q, ValueFact::of(Outcome::Null));
  EXPECT_EQ(state.pathGuard(), expected)
      << "a non-null from a dereference is not a condition a test made";

  state.nulls.set(X, NullRecord{.state = Nullness::NonNull,
                                .location = at(1),
                                .reason = NullReason::Tested,
                                .detail = {}});
  expected.require(X, ValueFact::of(Outcome::NonNull));
  EXPECT_EQ(state.pathGuard(), expected);
}

TEST(AnalysisState, JoinJoinsScalarsAndLearnRefutesAcrossTrackers) {
  // `if (c) { free(p); q = malloc(8); }` merged with the other arm.
  AnalysisState arm;
  arm.scalars.set(X, ValueFact::nonZero());
  arm.moves.markMoved(P, MoveReason::Freed, at(3), {}, ElementWitness::whole(),
                      "free", false, arm.pathGuard());
  arm.resources.hold(Q, ResourceRecord{.origin = ResourceOrigin::Allocated,
                                       .location = at(4),
                                       .family = "free",
                                       .escaped = false,
                                       .guard = arm.pathGuard()});
  AnalysisState other;
  other.scalars.set(X, ValueFact::of(Outcome::Zero));

  EXPECT_TRUE(arm.join(other));
  EXPECT_FALSE(arm.scalars.factOf(X)) << "zero or non-zero: nothing";
  EXPECT_TRUE(arm.moves.isMoved(P));
  EXPECT_TRUE(arm.resources.holds(Q));

  // The edge `!c`.
  const AnalysisState::Learned learned =
      arm.learn(X, ValueFact::of(Outcome::Zero));
  EXPECT_EQ(learned.reinstated, (std::vector<PlaceId>{P}));
  EXPECT_EQ(learned.cleared, (std::vector<PlaceId>{Q}));
  EXPECT_TRUE(learned.nullChanged.empty());
  EXPECT_FALSE(arm.moves.isMoved(P));
  EXPECT_FALSE(arm.resources.holds(Q));
}

TEST(AnalysisState, DropGuardsOnReachesEveryTracker) {
  AnalysisState state;
  PlaceGuard aboutX;
  aboutX.require(X, ValueFact::nonZero());
  state.moves.markMoved(P, MoveReason::Freed, at(3), {},
                        ElementWitness::whole(), "free", false, aboutX);
  state.resources.hold(Q, ResourceRecord{.origin = ResourceOrigin::Allocated,
                                         .location = at(4),
                                         .family = "free",
                                         .escaped = false,
                                         .guard = aboutX});
  state.nulls.set(Q, NullRecord{.state = Nullness::Null,
                                .location = at(5),
                                .reason = NullReason::AssignedNull,
                                .detail = {},
                                .guard = aboutX});
  state.dropGuardsOn(X);
  EXPECT_TRUE(state.moves.recordOf(P)->guard.trivial());
  EXPECT_TRUE(state.resources.recordOf(Q)->guard.trivial());
  EXPECT_TRUE(state.nulls.recordOf(Q)->guard.trivial());
  // Nothing was refuted; the records are all still there.
  EXPECT_TRUE(state.learn(X, ValueFact::of(Outcome::Zero)).reinstated.empty());
  EXPECT_TRUE(state.moves.isMoved(P));
}

} // namespace
} // namespace weavec::core
