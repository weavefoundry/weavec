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

  // RFC 0009, *Guards*: a class that frees p only under a guard on the
  // arguments does not free it whatever they are.
  FunctionSummary guarded = joined;
  PathGuard whenZero;
  whenZero.require(SummaryPath::param(1), ValueFact::of(Outcome::Zero));
  guarded.outcomes[Outcome::Zero][p].when = whenZero;
  EXPECT_FALSE(guarded.consumesUnconditionally(p))
      << "on the zero class p is freed only when the second argument is 0";

  FunctionSummary nothingKnown;
  nothingKnown.addEffect(p, PlaceEffect{.freed = true});
  EXPECT_TRUE(nothingKnown.consumesUnconditionally(p));
  // Without classes, the effect's own guard decides (Lua's `moveresults`
  // frees the stack `res` points into only for some `fwanted`): a copy of
  // `res` is not the callee's resource handed back.
  FunctionSummary guardedOnly;
  guardedOnly.addEffect(p, PlaceEffect{.freed = true, .when = whenZero});
  EXPECT_FALSE(guardedOnly.consumesUnconditionally(p));
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

// RFC 0009, *Argument-conditional summaries*.
TEST(FunctionSummary, GuardedAlternativesMergeByJoiningTheirGuards) {
  const auto when = [](const SummaryPath &path, const ValueFact &fact) {
    PathGuard guard;
    guard.require(path, fact);
    return guard;
  };
  const SummaryPath n = SummaryPath::param(3);

  // `if (nsize == 0) return NULL; ... return NULL;`: one `null` alternative
  // whose guard is what the two agree on (nothing).
  FunctionSummary s;
  ValueSource nullWhenZero = ValueSource::null();
  nullWhenZero.when = when(n, ValueFact::of(Outcome::Zero));
  s.addReturn(nullWhenZero);
  EXPECT_EQ(s.returns.size(), 1U);
  EXPECT_EQ(s.returns.begin()->when, when(n, ValueFact::of(Outcome::Zero)));
  ValueSource nullWhenPositive = ValueSource::null();
  nullWhenPositive.when = when(n, ValueFact::of(Outcome::Positive));
  s.addReturn(nullWhenPositive);
  EXPECT_EQ(s.returns.size(), 1U);
  EXPECT_EQ(s.returns.begin()->when,
            when(n, ValueFact::of({Outcome::Zero, Outcome::Positive})));
  s.addReturn(ValueSource::null());
  EXPECT_EQ(s.returns.size(), 1U);
  EXPECT_TRUE(s.returns.begin()->when.trivial());

  // A different kind is a different alternative, guard and all.
  ValueSource fresh = ValueSource::fresh("free");
  fresh.when = when(n, ValueFact::nonZero());
  s.addReturn(fresh);
  EXPECT_EQ(s.returns.size(), 2U);
  EXPECT_TRUE(s.returnsOnlyFresh());

  // Stores to one destination behave the same way.
  ValueSource copy = ValueSource::copy(SummaryPath::param(2));
  copy.when = when(SummaryPath::param(2), ValueFact::of(Outcome::NonNull));
  const SummaryPath dest = SummaryPath::param(0).deref().field("msg");
  s.addStore(Store{.dest = dest, .value = copy});
  s.addStore(Store{.dest = dest, .value = copy});
  EXPECT_EQ(s.stores.size(), 1U);
  EXPECT_EQ(s.stores.begin()->value.when, copy.when);
  s.addStore(
      Store{.dest = dest, .value = ValueSource::copy(SummaryPath::param(2))});
  EXPECT_EQ(s.stores.size(), 1U);
  EXPECT_TRUE(s.stores.begin()->value.when.trivial());

  // Effects: a consume guarded on both sides keeps the common conjuncts; a
  // side that consumes unconditionally makes the join unconditional.
  FunctionSummary a;
  a.addEffect(SummaryPath::param(1),
              PlaceEffect{.freed = true,
                          .family = "free",
                          .when = when(n, ValueFact::of(Outcome::Zero))});
  FunctionSummary b;
  b.addEffect(SummaryPath::param(1),
              PlaceEffect{.freed = true,
                          .family = "free",
                          .when = when(n, ValueFact::of(Outcome::Negative))});
  FunctionSummary joined = a;
  joined.join(b);
  EXPECT_EQ(joined.effectOf(SummaryPath::param(1)).when,
            when(n, ValueFact::of({Outcome::Zero, Outcome::Negative})));
  FunctionSummary c;
  c.addEffect(SummaryPath::param(1),
              PlaceEffect{.freed = true, .family = "free"});
  joined.join(c);
  EXPECT_TRUE(joined.effectOf(SummaryPath::param(1)).when.trivial());
  // A read merged into a guarded consume leaves the consume's guard alone:
  // the guard qualifies the consume only.
  a.addEffect(SummaryPath::param(1), PlaceEffect{.read = true});
  EXPECT_EQ(a.effectOf(SummaryPath::param(1)).when,
            when(n, ValueFact::of(Outcome::Zero)));
  EXPECT_TRUE(a.effectOf(SummaryPath::param(1)).read);
}

// RFC 0009, *Inferred `noreturn`*: the bit joins by conjunction, from the
// first candidate when the join starts empty.
TEST(FunctionSummary, NeverReturnsJoinsByConjunction) {
  FunctionSummary dies;
  dies.neverReturns = true;
  FunctionSummary returns;
  returns.addReturn(ValueSource::null());

  FunctionSummary both = dies;
  both.join(dies);
  EXPECT_TRUE(both.neverReturns);
  both.join(returns);
  EXPECT_FALSE(both.neverReturns);

  FunctionSummary fromBottom;
  fromBottom.join(dies);
  EXPECT_TRUE(fromBottom.neverReturns)
      << "the empty summary is bottom, not a candidate that returns";
  fromBottom.join(returns);
  EXPECT_FALSE(fromBottom.neverReturns);

  FunctionSummary other;
  other.join(returns);
  other.join(dies);
  EXPECT_FALSE(other.neverReturns) << "order does not matter";
}

// RFC 0010, *Shares*: `share` qualifies a consume and is a must-fact.
TEST(PlaceEffect, ShareJoinsByConjunction) {
  PlaceEffect release{.freed = true, .share = true, .family = "free"};
  PlaceEffect plain{.freed = true, .family = "free"};
  PlaceEffect joined = release;
  joined.join(plain);
  EXPECT_TRUE(joined.freed);
  EXPECT_FALSE(joined.share) << "a plain free on one path is a free";
  PlaceEffect both = release;
  both.join(release);
  EXPECT_TRUE(both.share);
}

// RFC 0011, *Derived pointers*: a consume records the offset it happened at
// (`free(container_of(p))` frees at `-struct outer.in`); a field offset and
// the start join to somewhere inside, and a side that did not consume takes
// the other's offset.
TEST(PlaceEffect, ConsumeOffsetsJoin) {
  const PointerOffset inner = PointerOffset::ofField("struct outer.in");
  PlaceEffect atInner{.freed = true, .family = "free", .at = inner.negated()};
  PlaceEffect atStart{.freed = true, .family = "free"};
  PlaceEffect joined = atInner;
  joined.join(atInner);
  EXPECT_EQ(joined.at, inner.negated());
  joined.join(atStart);
  EXPECT_TRUE(joined.at.isInside());
  PlaceEffect untouched{.read = true};
  untouched.join(atInner);
  EXPECT_EQ(untouched.at, inner.negated());
}

// RFC 0011, *Extents in summaries*: a requirement of either side is a
// requirement; the same need under two guards is one requirement under the
// joined guard; remapping globals carries the affine's path.
TEST(FunctionSummary, ExtentRequirementsJoinByUnion) {
  const PathAffine eight = PathAffine::ofConstant(8);
  const PathAffine n = PathAffine::ofPath(SummaryPath::param(1), 4, 0);
  FunctionSummary a;
  a.addRequirement(0, ExtentRequirement{.need = eight, .when = {}});
  FunctionSummary b;
  ExtentRequirement guarded{.need = n, .when = {}};
  guarded.when.require(SummaryPath::param(1), ValueFact::of(Outcome::Positive));
  b.addRequirement(0, guarded);
  a.join(b);
  ASSERT_EQ(a.requiresExtent.at(0).size(), 2U);
  EXPECT_TRUE(a.requiresExtent.at(0).contains(
      ExtentRequirement{.need = eight, .when = {}}));
  EXPECT_TRUE(a.requiresExtent.at(0).contains(guarded));

  FunctionSummary c;
  c.addRequirement(0, guarded);
  ExtentRequirement otherwise{.need = n, .when = {}};
  otherwise.when.require(SummaryPath::param(1), ValueFact::of(Outcome::Zero));
  c.addRequirement(0, otherwise);
  ASSERT_EQ(c.requiresExtent.at(0).size(), 1U);
  EXPECT_EQ(c.requiresExtent.at(0).begin()->need, n);
  const PathGuard &when = c.requiresExtent.at(0).begin()->when;
  ASSERT_EQ(when.conditions.size(), 1U);
  EXPECT_EQ(when.conditions.at(SummaryPath::param(1)).classes,
            (OutcomeSet{Outcome::Positive, Outcome::Zero}));
}

// RFC 0010, *Stores out of sight*: `escaped` is an effect of its own (an
// `effects` entry with nothing else is kept) and a may-fact.
TEST(PlaceEffect, EscapedIsAMayFact) {
  PlaceEffect escaped{.escaped = true};
  EXPECT_FALSE(escaped.empty());
  EXPECT_FALSE(escaped.consumed());
  PlaceEffect read{.read = true};
  read.join(escaped);
  EXPECT_TRUE(read.escaped);
  EXPECT_TRUE(read.read);
  PlaceEffect joined = escaped;
  joined.join(PlaceEffect{.written = true});
  EXPECT_TRUE(joined.escaped) << "the other side's silence does not undo it";

  FunctionSummary summary;
  summary.addEffect(SummaryPath::param(1), escaped);
  EXPECT_TRUE(summary.effectOf(SummaryPath::param(1)).escaped);
  EXPECT_FALSE(summary.consumes(1));
  EXPECT_EQ(summary.inferredKind(1), OwnershipKind::Unknown)
      << "escaping says nothing about ownership";
}

// RFC 0010: increments, decrements and counts are may-facts; `retains` asks
// about a parameter's pointee.
TEST(FunctionSummary, IncrementsJoinByUnionAndRetains) {
  FunctionSummary ref;
  ref.increments.insert(SummaryPath::param(0).deref().field("rc"));
  EXPECT_TRUE(ref.retains(0));
  EXPECT_FALSE(ref.retains(1));
  EXPECT_FALSE(ref.empty());

  FunctionSummary unref;
  unref.decrements.insert(SummaryPath::param(1).deref().field("rc"));
  unref.counts.insert(SummaryPath::param(1).deref().field("rc"));
  unref.addEffect(SummaryPath::param(1),
                  PlaceEffect{.freed = true, .share = true, .family = "free"});

  FunctionSummary both = ref;
  both.join(unref);
  EXPECT_EQ(both.increments.size(), 1U);
  EXPECT_EQ(both.decrements.size(), 1U);
  EXPECT_EQ(both.counts.size(), 1U);
  EXPECT_TRUE(both.retains(0));
  EXPECT_TRUE(both.effectOf(SummaryPath::param(1)).freed);
  EXPECT_TRUE(both.effectOf(SummaryPath::param(1)).share)
      << "consumed on one side only: whenever it is, it is a share release";

  FunctionSummary plain;
  plain.addEffect(SummaryPath::param(1),
                  PlaceEffect{.freed = true, .family = "free"});
  FunctionSummary mixed = unref;
  mixed.join(plain);
  EXPECT_FALSE(mixed.effectOf(SummaryPath::param(1)).share)
      << "a plain free on some path: the caller's other shares may be dead";
}

// RFC 0010, *Per-outcome stores*: a class stores what some path returning it
// stores; a map that says every class stores everything is dropped.
TEST(FunctionSummary, StoresOnClassAndNormalisation) {
  const SummaryPath dest = SummaryPath::param(0).deref().field("items");
  FunctionSummary put;
  put.addStore(
      Store{.dest = dest, .value = ValueSource::copy(SummaryPath::param(1))});
  put.addOutcome(Outcome::Zero);
  put.addOutcome(Outcome::Negative);
  put.storesOn[Outcome::Zero] = {dest};
  put.storesOn[Outcome::Negative] = {};

  EXPECT_EQ(put.storesOnClass(Outcome::Zero), (std::set<SummaryPath>{dest}));
  EXPECT_TRUE(put.storesOnClass(Outcome::Negative).empty());
  EXPECT_EQ(put.storesOnClass(Outcome::Positive), (std::set<SummaryPath>{dest}))
      << "an unknown class stores everything";
  put.normalizeStoresOn();
  EXPECT_EQ(put.storesOn.size(), 2U) << "the classes differ: kept";

  FunctionSummary always;
  always.addStore(Store{.dest = dest, .value = ValueSource::null()});
  always.addOutcome(Outcome::Zero);
  always.addOutcome(Outcome::Negative);
  always.storesOn[Outcome::Zero] = {dest};
  always.storesOn[Outcome::Negative] = {dest};
  always.normalizeStoresOn();
  EXPECT_TRUE(always.storesOn.empty()) << "uniform: says nothing";
  EXPECT_EQ(always.storesOnClass(Outcome::Negative),
            (std::set<SummaryPath>{dest}));

  // The join is a per-class union: a path that stores on `negative` on the
  // other side makes `negative` a storing class.
  FunctionSummary joined = put;
  joined.join(always);
  EXPECT_TRUE(joined.storesOn.empty())
      << "after the join every class stores to the only destination";

  // A side without outcome knowledge wipes the per-class maps.
  FunctionSummary unknown;
  unknown.addStore(Store{.dest = dest, .value = ValueSource::null()});
  FunctionSummary wiped = put;
  wiped.join(unknown);
  EXPECT_TRUE(wiped.storesOn.empty());
  EXPECT_TRUE(wiped.outcomes.empty());
}

// RFC 0010, *Per-outcome integer facts*: a must-fact per class.
TEST(FunctionSummary, FactsOnClassJoinByIntersection) {
  const SummaryPath n = SummaryPath::param(0).deref().field("n");
  const SummaryPath m = SummaryPath::param(0).deref().field("m");
  FunctionSummary a;
  a.addOutcome(Outcome::Zero);
  a.addOutcome(Outcome::Negative);
  a.factOn[Outcome::Zero] = {{n, ValueFact::of(Outcome::Positive)},
                             {m, ValueFact::ofConstant(1)}};
  a.factOn[Outcome::Negative] = {{n, ValueFact::of(Outcome::Zero)}};

  FunctionSummary b;
  b.addOutcome(Outcome::Zero);
  b.addOutcome(Outcome::Positive);
  b.factOn[Outcome::Zero] = {{n, ValueFact::ofConstant(2)}};
  b.factOn[Outcome::Positive] = {{m, ValueFact::ofConstant(3)}};

  FunctionSummary joined = a;
  joined.join(b);
  ASSERT_TRUE(joined.factOn.contains(Outcome::Zero));
  EXPECT_EQ(joined.factOn[Outcome::Zero].size(), 1U) << "`m` on one side only";
  EXPECT_EQ(joined.factOn[Outcome::Zero].at(n),
            ValueFact::of(Outcome::Positive))
      << "positive joined with the constant 2 is positive";
  EXPECT_EQ(joined.factOn[Outcome::Negative].at(n),
            ValueFact::of(Outcome::Zero))
      << "a class only one side returns keeps that side's facts";
  EXPECT_EQ(joined.factOn[Outcome::Positive].at(m), ValueFact::ofConstant(3));
  EXPECT_FALSE(joined.empty());

  // Remapping keeps the per-class facts and stores on kept roots.
  FunctionSummary global;
  global.addOutcome(Outcome::Zero);
  global.addOutcome(Outcome::Positive);
  global.factOn[Outcome::Zero] = {
      {SummaryPath::global(4), ValueFact::ofConstant(0)}};
  global.addStore(
      Store{.dest = SummaryPath::global(4), .value = ValueSource::null()});
  global.storesOn[Outcome::Zero] = {SummaryPath::global(4)};
  global.storesOn[Outcome::Positive] = {};
  global.increments.insert(SummaryPath::global(4).deref().field("rc"));
  const FunctionSummary mapped = remapGlobals(global, [](std::uint32_t id) {
    return std::optional<std::uint32_t>(id + 10);
  });
  EXPECT_TRUE(
      mapped.factOn.at(Outcome::Zero).contains(SummaryPath::global(14)));
  EXPECT_TRUE(
      mapped.storesOn.at(Outcome::Zero).contains(SummaryPath::global(14)));
  EXPECT_TRUE(
      mapped.increments.contains(SummaryPath::global(14).deref().field("rc")));
  const FunctionSummary dropped = remapGlobals(
      global, [](std::uint32_t) { return std::optional<std::uint32_t>(); });
  EXPECT_TRUE(dropped.factOn.empty());
  EXPECT_TRUE(dropped.increments.empty());
  EXPECT_TRUE(dropped.storesOn.empty()) << "no destinations left: uniform";
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
