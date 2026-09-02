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
  b.reallocLike = true;

  FunctionSummary joined = a;
  joined.join(b);
  EXPECT_EQ(joined.effectOf(p), (PlaceEffect{.read = true, .freed = true}));
  EXPECT_EQ(joined.effectOf(p.deref()), (PlaceEffect{.written = true}));
  EXPECT_EQ(joined.stores.size(), 1U);
  EXPECT_EQ(joined.returns.size(), 2U);
  EXPECT_TRUE(joined.reallocLike);

  // Idempotent and commutative.
  FunctionSummary again = joined;
  again.join(a);
  again.join(b);
  EXPECT_EQ(again, joined);
  FunctionSummary other = b;
  other.join(a);
  EXPECT_EQ(other, joined);
}

TEST(ValueSource, KindNames) {
  EXPECT_EQ(toString(ValueSource::Kind::Fresh), "fresh");
  EXPECT_EQ(toString(ValueSource::Kind::Copy), "copy");
  EXPECT_EQ(toString(ValueSource::Kind::Borrow), "borrow");
  EXPECT_EQ(toString(ValueSource::Kind::Null), "null");
  EXPECT_EQ(toString(ValueSource::Kind::Unknown), "unknown");
}

} // namespace
} // namespace weavec::core
