//===- SummaryTest.cpp - Tests for function summaries ---------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Summary.h"

#include <gtest/gtest.h>

namespace weavec::core {
namespace {

TEST(SummaryPath, SpellsLikePlaces) {
  const SummaryPath p = SummaryPath::param(0);
  EXPECT_EQ(p.toString("p"), "p");
  EXPECT_EQ(p.deref().toString("p"), "*p");
  EXPECT_EQ(p.deref().field("data").toString("b"), "b->data");
  EXPECT_EQ(p.field("f").toString("s"), "s.f");
  EXPECT_EQ(p.indexed().toString("a"), "a[*]");
  EXPECT_EQ(p.deref().field("next").deref().field("v").toString("n"),
            "n->next->v");
  EXPECT_EQ(p.deref().deref().toString("pp"), "**pp");
}

TEST(SummaryPath, IndexCollapsesOntoDerefAndIndex) {
  const SummaryPath p = SummaryPath::param(1);
  EXPECT_EQ(p.deref().indexed(), p.deref());
  EXPECT_EQ(p.indexed().indexed(), p.indexed());
  EXPECT_NE(p.field("a").indexed(), p.field("a"));
}

TEST(SummaryPath, PrefixAndDeref) {
  const SummaryPath p = SummaryPath::param(0);
  EXPECT_TRUE(p.isProperPrefixOf(p.deref()));
  EXPECT_TRUE(p.deref().isProperPrefixOf(p.deref().field("x")));
  EXPECT_FALSE(p.isProperPrefixOf(p));
  EXPECT_FALSE(p.deref().isProperPrefixOf(SummaryPath::param(1).deref()));
  EXPECT_FALSE(p.isProperPrefixOf(SummaryPath::global(0)));
  EXPECT_FALSE(p.hasDeref());
  EXPECT_FALSE(p.field("f").hasDeref());
  EXPECT_TRUE(p.deref().hasDeref());
  EXPECT_EQ(p.deref().field("f").rootPath(), p);
  EXPECT_TRUE(p.isRoot());
  EXPECT_FALSE(p.deref().isRoot());
  EXPECT_TRUE(p.isParam());
  EXPECT_FALSE(SummaryPath::global(2).isParam());
}

TEST(SummaryPath, OrdersDeterministically) {
  EXPECT_LT(SummaryPath::param(0), SummaryPath::param(1));
  EXPECT_LT(SummaryPath::param(0), SummaryPath::param(0).deref());
  EXPECT_LT(SummaryPath::param(5), SummaryPath::global(0));
}

TEST(PlaceEffect, JoinIsOr) {
  PlaceEffect a{.read = true};
  const PlaceEffect b{.freed = true};
  a.join(b);
  EXPECT_TRUE(a.read);
  EXPECT_TRUE(a.freed);
  EXPECT_FALSE(a.written);
  EXPECT_TRUE(a.consumed());
  EXPECT_TRUE(a.mutates());
  EXPECT_FALSE(PlaceEffect{.read = true}.mutates());
  EXPECT_TRUE(PlaceEffect{}.empty());
}

// RFC 0008, *Replaced values*: `replaced` is a must-fact about a consume.
TEST(PlaceEffect, ReplacedIsAMustFactOfTheConsume) {
  // Both sides consume and replace: replaced.
  PlaceEffect both{.freed = true, .replaced = true};
  both.join(PlaceEffect{.moved = true, .replaced = true});
  EXPECT_TRUE(both.replaced);
  EXPECT_TRUE(both.consumed());

  // One side may leave the consumed value in place: not replaced.
  PlaceEffect mixed{.freed = true, .replaced = true};
  mixed.join(PlaceEffect{.freed = true});
  EXPECT_FALSE(mixed.replaced);

  // The other way round too.
  PlaceEffect mixed2{.freed = true};
  mixed2.join(PlaceEffect{.freed = true, .replaced = true});
  EXPECT_FALSE(mixed2.replaced);

  // A side that does not consume says nothing about replacement.
  PlaceEffect written{.written = true};
  written.join(PlaceEffect{.freed = true, .replaced = true});
  EXPECT_TRUE(written.replaced);
  EXPECT_TRUE(written.written);
  PlaceEffect consumed{.freed = true, .replaced = true};
  consumed.join(PlaceEffect{.read = true});
  EXPECT_TRUE(consumed.replaced);
}

// RFC 0008, *Element consumes*: `element` holds only if every consuming
// side went through an element access; a whole consume on either side wins.
TEST(PlaceEffect, ElementIsAMustFactOfTheConsume) {
  PlaceEffect both{.freed = true, .element = true};
  both.join(PlaceEffect{.freed = true, .element = true});
  EXPECT_TRUE(both.element);

  PlaceEffect mixed{.freed = true, .element = true};
  mixed.join(PlaceEffect{.freed = true});
  EXPECT_FALSE(mixed.element);

  PlaceEffect mixed2{.moved = true};
  mixed2.join(PlaceEffect{.freed = true, .element = true});
  EXPECT_FALSE(mixed2.element);

  // A side that does not consume says nothing about elements.
  PlaceEffect written{.written = true};
  written.join(PlaceEffect{.freed = true, .element = true});
  EXPECT_TRUE(written.element);
  PlaceEffect consumed{.freed = true, .element = true};
  consumed.join(PlaceEffect{.read = true});
  EXPECT_TRUE(consumed.element);
}

// RFC 0008, *Nullness*: `requires` unions, `notnull` intersects per class,
// and a `null` return alternative is what `mayReturnNull` reports.
TEST(FunctionSummary, NullnessFactsJoin) {
  FunctionSummary a;
  a.requiresNonNull.insert(0);
  a.addReturn(ValueSource::fresh("free"));
  a.addOutcome(Outcome::Zero);
  a.addOutcome(Outcome::Negative);
  a.nonNullOn[Outcome::Zero].insert(SummaryPath::param(1).deref());
  a.nonNullOn[Outcome::Zero].insert(SummaryPath::param(2).deref());
  a.nonNullOn[Outcome::Negative].insert(SummaryPath::param(1).deref());
  EXPECT_FALSE(a.mayReturnNull());
  EXPECT_TRUE(a.requiresParam(0));
  EXPECT_FALSE(a.requiresParam(1));

  FunctionSummary b;
  b.requiresNonNull.insert(1);
  b.addReturn(ValueSource::null());
  b.addOutcome(Outcome::Zero);
  b.addOutcome(Outcome::Positive);
  b.nonNullOn[Outcome::Zero].insert(SummaryPath::param(1).deref());
  b.nonNullOn[Outcome::Positive].insert(SummaryPath::param(3).deref());

  a.join(b);
  EXPECT_TRUE(a.mayReturnNull());
  EXPECT_EQ(a.requiresNonNull, (std::set<std::uint32_t>{0, 1}))
      << "a parameter either side dereferences";
  // Zero: both may return it, keep what both guarantee.
  EXPECT_EQ(a.nonNullOn.at(Outcome::Zero),
            std::set<SummaryPath>{SummaryPath::param(1).deref()});
  // Negative / Positive: only one side returns it, its facts hold there.
  EXPECT_EQ(a.nonNullOn.at(Outcome::Negative),
            std::set<SummaryPath>{SummaryPath::param(1).deref()});
  EXPECT_EQ(a.nonNullOn.at(Outcome::Positive),
            std::set<SummaryPath>{SummaryPath::param(3).deref()});

  // A side without outcome knowledge drops every class fact.
  FunctionSummary c;
  c.addReturn(ValueSource::fresh("free"));
  a.join(c);
  EXPECT_TRUE(a.nonNullOn.empty());
  EXPECT_TRUE(a.outcomes.empty());
  EXPECT_EQ(a.requiresNonNull, (std::set<std::uint32_t>{0, 1}));
}

TEST(FunctionSummary, ResultRootedStoresSurviveRemapping) {
  // `result .p` names a field of a struct returned by value (RFC 0008,
  // *Struct-by-value results*); it is not a global and is not renumbered.
  FunctionSummary s;
  s.addStore(Store{.dest = SummaryPath::result().field("p"),
                   .value = ValueSource::fresh("free")});
  s.addStore(Store{.dest = SummaryPath::global(4),
                   .value = ValueSource::copy(SummaryPath::param(0))});
  s.requiresNonNull.insert(0);
  s.addOutcome(Outcome::Zero);
  s.nonNullOn[Outcome::Zero].insert(SummaryPath::param(1).deref());
  const FunctionSummary remapped =
      remapGlobals(s, [](std::uint32_t) -> std::optional<std::uint32_t> {
        return std::nullopt;
      });
  EXPECT_TRUE(remapped.storesTo(SummaryPath::result().field("p")));
  EXPECT_FALSE(remapped.storesTo(SummaryPath::global(4)))
      << "a declined global is dropped";
  EXPECT_TRUE(remapped.requiresParam(0));
  EXPECT_EQ(remapped.nonNullOn.at(Outcome::Zero),
            std::set<SummaryPath>{SummaryPath::param(1).deref()});
}

TEST(FunctionSummary, EmptyEffectsAreNotStored) {
  FunctionSummary s;
  s.addEffect(SummaryPath::param(0), PlaceEffect{});
  EXPECT_TRUE(s.effects.empty());
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.effectOf(SummaryPath::param(0)), PlaceEffect{});
}

TEST(FunctionSummary, ConsumesAndFrees) {
  FunctionSummary frees;
  frees.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  EXPECT_TRUE(frees.consumes(0));
  EXPECT_TRUE(frees.frees(0));
  EXPECT_FALSE(frees.consumes(1));
  EXPECT_EQ(frees.inferredKind(0), OwnershipKind::Owned);

  FunctionSummary moves;
  moves.addEffect(SummaryPath::param(0), PlaceEffect{.moved = true});
  EXPECT_TRUE(moves.consumes(0));
  EXPECT_FALSE(moves.frees(0));
  EXPECT_EQ(moves.inferredKind(0), OwnershipKind::Owned);
}

TEST(FunctionSummary, BorrowKindFromPointeeEffects) {
  const SummaryPath p = SummaryPath::param(0);

  FunctionSummary reads;
  reads.addEffect(p.deref().field("v"), PlaceEffect{.read = true});
  EXPECT_EQ(reads.borrowKind(0), BorrowKind::Shared);
  EXPECT_EQ(reads.inferredKind(0), OwnershipKind::Shared);

  FunctionSummary writes;
  writes.addEffect(p.deref(), PlaceEffect{.written = true});
  EXPECT_EQ(writes.borrowKind(0), BorrowKind::Mutable);
  EXPECT_EQ(writes.inferredKind(0), OwnershipKind::Mutable);

  FunctionSummary freesField;
  freesField.addEffect(p.deref().field("data"), PlaceEffect{.freed = true});
  EXPECT_EQ(freesField.borrowKind(0), BorrowKind::Mutable);
  EXPECT_FALSE(freesField.consumes(0)) << "freeing a field is not consuming";

  FunctionSummary storesInto;
  storesInto.addStore(
      Store{.dest = p.deref().field("data"), .value = ValueSource::fresh()});
  EXPECT_EQ(storesInto.borrowKind(0), BorrowKind::Mutable);

  FunctionSummary onlyRootRead;
  onlyRootRead.addEffect(p, PlaceEffect{.read = true});
  EXPECT_EQ(onlyRootRead.borrowKind(0), std::nullopt)
      << "reading the pointer value itself is not a borrow of the pointee";
  EXPECT_EQ(onlyRootRead.inferredKind(0), OwnershipKind::Unknown);

  FunctionSummary otherParam;
  otherParam.addEffect(SummaryPath::param(1).deref(),
                       PlaceEffect{.read = true});
  EXPECT_EQ(otherParam.borrowKind(0), std::nullopt);
}

TEST(FunctionSummary, ReturnKind) {
  FunctionSummary fresh;
  fresh.addReturn(ValueSource::fresh());
  fresh.addReturn(ValueSource::null());
  EXPECT_EQ(fresh.inferredReturnKind(), OwnershipKind::Owned);

  FunctionSummary borrow;
  borrow.addReturn(ValueSource::borrow(SummaryPath::param(0).deref()));
  EXPECT_EQ(borrow.inferredReturnKind(), OwnershipKind::Shared);

  FunctionSummary mixed;
  mixed.addReturn(ValueSource::fresh());
  mixed.addReturn(ValueSource::copy(SummaryPath::param(0)));
  EXPECT_EQ(mixed.inferredReturnKind(), OwnershipKind::Unknown);

  FunctionSummary unknown;
  unknown.addReturn(ValueSource::fresh());
  unknown.addReturn(ValueSource::unknown());
  EXPECT_EQ(unknown.inferredReturnKind(), OwnershipKind::Unknown);

  // RFC 0004: a raw alternative makes the whole result raw.
  FunctionSummary raw;
  raw.addReturn(ValueSource::fresh());
  raw.addReturn(ValueSource::raw());
  EXPECT_EQ(raw.inferredReturnKind(), OwnershipKind::Raw);

  EXPECT_EQ(FunctionSummary{}.inferredReturnKind(), OwnershipKind::Unknown);
}

TEST(FunctionSummary, JoinIsUnion) {
  const SummaryPath p = SummaryPath::param(0);
  FunctionSummary a;
  a.addEffect(p, PlaceEffect{.freed = true});
  a.addReturn(ValueSource::fresh());

  FunctionSummary b;
  b.addEffect(p, PlaceEffect{.read = true});
  b.addEffect(p.deref(), PlaceEffect{.written = true});
  b.addStore(
      Store{.dest = SummaryPath::global(0), .value = ValueSource::copy(p)});
  b.addReturn(ValueSource::null());

  FunctionSummary joined = a;
  joined.join(b);
  EXPECT_EQ(joined.effectOf(p), (PlaceEffect{.read = true, .freed = true}));
  EXPECT_EQ(joined.effectOf(p.deref()), (PlaceEffect{.written = true}));
  EXPECT_EQ(joined.stores.size(), 1U);
  EXPECT_EQ(joined.returns.size(), 2U);

  // Idempotent and commutative.
  FunctionSummary again = joined;
  again.join(a);
  again.join(b);
  EXPECT_EQ(again, joined);
  FunctionSummary other = b;
  other.join(a);
  EXPECT_EQ(other, joined);
}

// RFC 0006, *Outcome-conditional summaries*.
TEST(FunctionSummary, OutcomesJoinPerClassAndDecideConditionality) {
  const SummaryPath p = SummaryPath::param(0);
  FunctionSummary a;
  a.addEffect(p, PlaceEffect{.freed = true});
  a.addOutcome(Outcome::Zero, p, PlaceEffect{.freed = true});
  a.addOutcome(Outcome::Negative);
  EXPECT_FALSE(a.consumesUnconditionally(p))
      << "a negative return keeps the argument";

  FunctionSummary b;
  b.addEffect(p, PlaceEffect{.freed = true});
  b.addOutcome(Outcome::Negative, p, PlaceEffect{.freed = true});
  FunctionSummary joined = a;
  joined.join(b);
  EXPECT_TRUE(joined.consumesUnconditionally(p))
      << "every class frees p once the negative path does too";
  EXPECT_EQ(joined.outcomes.size(), 2U);

  FunctionSummary nothingKnown;
  nothingKnown.addEffect(p, PlaceEffect{.freed = true});
  EXPECT_TRUE(nothingKnown.consumesUnconditionally(p));
  FunctionSummary mixed = a;
  mixed.join(nothingKnown);
  EXPECT_TRUE(mixed.outcomes.empty())
      << "a side without outcome knowledge makes the join unconditional";

  FunctionSummary fromBottom;
  fromBottom.join(a);
  EXPECT_EQ(fromBottom.outcomes, a.outcomes)
      << "the empty summary is bottom: joining candidates into it keeps "
         "the first candidate's classes";
  EXPECT_FALSE(fromBottom.consumesUnconditionally(p));

  EXPECT_EQ(toString(Outcome::Null), "null");
  EXPECT_EQ(toString(Outcome::NonNull), "nonnull");
  EXPECT_EQ(toString(Outcome::Zero), "zero");
  EXPECT_EQ(toString(Outcome::Positive), "positive");
  EXPECT_EQ(toString(Outcome::Negative), "negative");
  EXPECT_EQ(parseOutcome("nonnull"), Outcome::NonNull);
  EXPECT_FALSE(parseOutcome("maybe"));
}

TEST(ValueSource, KindNames) {
  EXPECT_EQ(toString(ValueSource::Kind::Fresh), "fresh");
  EXPECT_EQ(toString(ValueSource::Kind::Copy), "copy");
  EXPECT_EQ(toString(ValueSource::Kind::Borrow), "borrow");
  EXPECT_EQ(toString(ValueSource::Kind::Null), "null");
  EXPECT_EQ(toString(ValueSource::Kind::Unknown), "unknown");
  EXPECT_EQ(toString(ValueSource::Kind::Raw), "raw");
}

} // namespace
} // namespace weavec::core
