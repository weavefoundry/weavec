//===- CallTargetsTest.cpp - RFC 0014 target sets and pointer guards ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/CallTargets.h"

#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/SummaryIO.h"

#include <gtest/gtest.h>

namespace weavec::core {

TEST(CallTargets, JoiningPreservesUnknownAndKnownAlternatives) {
  auto targets = CallTargets::function("release");
  EXPECT_TRUE(targets.join(CallTargets::any()));
  EXPECT_TRUE(targets.unknown);
  EXPECT_TRUE(targets.functions.contains("release"));
  EXPECT_FALSE(targets.resolved());
  EXPECT_FALSE(targets.join(CallTargets::function("release")));
}

TEST(CallTargets, NullIsAnIndependentAlternative) {
  auto targets = CallTargets::function("keep");
  EXPECT_TRUE(targets.resolved());
  targets.join(CallTargets{.functions = {}, .unknown = false, .null = true});
  EXPECT_FALSE(targets.resolved());
  EXPECT_FALSE(targets.unknown);
  EXPECT_TRUE(targets.null);
}

TEST(CallTargets, OverflowRetainsAnExplicitUnknownAndADeterministicPrefix) {
  CallTargets forward, reverse;
  for (unsigned i = 0; i < 2 * MaxCallTargets; ++i) {
    forward.join(CallTargets::function("function" + std::to_string(i)));
    reverse.join(CallTargets::function(
        "function" + std::to_string(2 * MaxCallTargets - i - 1)));
  }
  EXPECT_EQ(forward, reverse);
  EXPECT_EQ(forward.functions.size(), MaxCallTargets);
  EXPECT_TRUE(forward.unknown);
}

TEST(CallTargets, SymbolsIncludingUnitPathsRoundTrip) {
  auto targets = CallTargets::function("/work/a dir/callback.c#release");
  targets.join(CallTargets::function("same_name"));
  targets.unknown = true;
  targets.null = true;
  EXPECT_EQ(CallTargets::parse(targets.toString()), targets);
  EXPECT_NE(CallTargets::function("a.c#release"),
            CallTargets::function("b.c#release"));
}

TEST(CallTargets, MalformedAndDuplicateSymbolsAreRejected) {
  for (const auto *text : {"", "garbage", "-:", "-:0", "-:gg", "-:00", "-:0a",
                           "-:61:61", "?0extra"})
    EXPECT_FALSE(CallTargets::parse(text)) << text;
}

TEST(CallTargets, ATargetMissingOnOneIncomingEdgeBecomesUnknown) {
  AnalysisState left, right;
  const PlaceId callback{1};
  left.callTargets[callback] = CallTargets::function("drop");
  EXPECT_TRUE(left.join(right));
  EXPECT_TRUE(left.callTargets.at(callback).unknown);
  EXPECT_TRUE(left.callTargets.at(callback).functions.contains("drop"));
  left.forget(callback);
  EXPECT_FALSE(left.callTargets.contains(callback));
}

TEST(PointerGuard, PairsAreCanonicalAndReflexivityIsKnown) {
  PlaceGuard guard;
  const PlaceId a{1}, b{2};
  guard.requirePointer(b, a, false);
  EXPECT_EQ(guard.pointerFact(a, b), false);
  EXPECT_EQ(guard.pointerFact(a, a), true);
  EXPECT_EQ(guard.pointers.size(), 1U);
  EXPECT_EQ(guard.pointers.begin()->first, (std::pair{a, b}));
}

TEST(PointerGuard, JoinsKeepOnlyAgreedPredicates) {
  PlaceGuard left, right;
  const PlaceId a{1}, b{2}, c{3};
  left.requirePointer(a, b, false);
  left.requirePointer(b, c, true);
  right.requirePointer(b, a, false);
  right.requirePointer(b, c, false);
  EXPECT_TRUE(left.join(right));
  EXPECT_EQ(left.pointerFact(a, b), false);
  EXPECT_FALSE(left.pointerFact(b, c));
}

TEST(PointerGuard, RebindingDropsOnlyPredicatesAboutTheOldValue) {
  PlaceGuard guard;
  const PlaceId a{1}, b{2}, saved{3};
  guard.requirePointer(a, b, false);
  guard.copyPointer(a, saved);
  guard.drop(a);
  EXPECT_FALSE(guard.pointerFact(a, b));
  EXPECT_EQ(guard.pointerFact(saved, b), false);
}

TEST(PointerGuard, SummaryMetadataAndPredicatesRoundTrip) {
  FunctionSummary summary;
  const auto p = SummaryPath::param(0), q = SummaryPath::param(1);
  PlaceEffect effect{.freed = true};
  effect.when.requirePointer(p, q, true);
  summary.addEffect(p, effect);
  summary.callbackInputs.insert(
      SummaryPath::param(2).deref().field("callback"));
  summary.objectViews[p.deref()] = "layout-key";
  summary.incomplete.insert("indirect call has an unresolved target");
  summary.addReturn(
      ValueSource::function(CallTargets::function("a.c#callback")));
  const auto printed =
      printSummary(summary, [](std::uint32_t) { return std::string{}; });
  const auto parsed = parseSummary(
      printed, [](std::string_view) { return std::optional<std::uint32_t>{}; });
  ASSERT_TRUE(parsed) << printed;
  EXPECT_EQ(*parsed, summary);
}

TEST(PointerGuard, DeclinedGlobalPredicatesWeakenTheGuard) {
  FunctionSummary summary;
  PlaceEffect effect{.freed = true};
  effect.when.requirePointer(SummaryPath::param(0), SummaryPath::global(0),
                             true);
  summary.addEffect(SummaryPath::param(0), effect);
  const auto remapped = remapGlobals(
      summary, [](std::uint32_t) { return std::optional<std::uint32_t>{}; });
  EXPECT_TRUE(remapped.effectOf(SummaryPath::param(0)).freed);
  EXPECT_TRUE(remapped.effectOf(SummaryPath::param(0)).when.trivial());
}

TEST(CallTargets, ContextBindingsRoundTripAndRejectUnboundedPaths) {
  const CallbackBindings bindings{
      {SummaryPath::param(0), CallTargets::function("drop")},
      {SummaryPath::param(1).deref().field("fn"),
       CallTargets::function("src.c#keep")}};
  EXPECT_EQ(parseCallbackBindings(printCallbackBindings(bindings)), bindings);
  EXPECT_FALSE(parseCallbackBindings(""));
  EXPECT_FALSE(parseCallbackBindings("param~0=-:61;param~0=-:62"));
  EXPECT_FALSE(parseCallbackBindings("global~x=-:61"));
  EXPECT_FALSE(parseCallbackBindings("param~0=-:61;"));
}

} // namespace weavec::core
