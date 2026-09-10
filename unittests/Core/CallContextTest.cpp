//===- CallContextTest.cpp - RFC 0016 portable caller contexts ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/CallContext.h"

#include "weavec/Core/AnalysisState.h"

#include <gtest/gtest.h>

namespace weavec::core {

static std::string contextGlobalName(std::uint32_t id) {
  return "global_" + std::to_string(id);
}
static std::optional<std::uint32_t>
resolveContextGlobal(std::string_view name) {
  if (name == "global_0")
    return 0;
  if (name == "global_1")
    return 1;
  return std::nullopt;
}

static CallContext equalInputs() {
  CallContext result;
  EXPECT_TRUE(result.addAlias({.first = SummaryPath::param(0),
                               .second = SummaryPath::param(1),
                               .offset = {}}));
  return result;
}

TEST(CallContext, TraversalOrdersRoundTripRemapAndRequireDefiniteIdentity) {
  const auto a = SummaryPath::param(0).deref();
  const auto b = SummaryPath::global(0);
  CallContext input;
  EXPECT_TRUE(input.addAlias({.first = a,
                              .second = b,
                              .offset = PointerOffset::unknown(),
                              .definite = true,
                              .sameShare = false}));
  input.orders.emplace(a, b);
  EXPECT_TRUE(input.valid());
  const auto encoded = printCallContext(input, contextGlobalName);
  EXPECT_EQ(parseCallContext(encoded, resolveContextGlobal), input);
  const auto mapped =
      remapCallContext(input, [](std::uint32_t) { return std::optional(1U); });
  ASSERT_TRUE(mapped);
  EXPECT_TRUE(mapped->orders.contains({a, SummaryPath::global(1)}));
  EXPECT_FALSE(remapCallContext(
      input, [](std::uint32_t) { return std::optional<std::uint32_t>{}; }));
  auto bad = input;
  bad.aliases.clear();
  EXPECT_FALSE(bad.valid());
  bad = input;
  auto alias = *bad.aliases.begin();
  bad.aliases.clear();
  alias.definite = false;
  bad.aliases.insert(alias);
  EXPECT_FALSE(bad.valid());
  bad = input;
  bad.orders.emplace(a, a);
  EXPECT_FALSE(bad.valid());
  bad = input;
  bad.facts[b] = ValueFact::of(Outcome::Null);
  EXPECT_FALSE(bad.valid());
  const auto order = encoded.substr(encoded.find(";o:"));
  EXPECT_FALSE(parseCallContext(encoded + order, resolveContextGlobal));
}

TEST(CallContext, TraversalOrderClosureRejectsContradictoryExactOffsets) {
  const auto a = SummaryPath::param(0);
  const auto b = SummaryPath::param(1);
  const auto c = SummaryPath::param(2);
  CallContext input;
  EXPECT_TRUE(input.addAlias(
      {.first = a, .second = b, .offset = PointerOffset::ofElements(1)}));
  EXPECT_TRUE(input.addAlias(
      {.first = b, .second = c, .offset = PointerOffset::unknown()}));
  input.orders.emplace(a, c);
  EXPECT_TRUE(input.valid());
  input.orders.emplace(c, b);
  EXPECT_FALSE(input.valid());
  input.orders.clear();
  input.orders.emplace(b, a);
  EXPECT_TRUE(input.valid());
  input.orders.emplace(a, b);
  EXPECT_FALSE(input.valid());
}

TEST(CallContext, CanonicalOrderingReversesTheOffset) {
  CallContext forward;
  CallContext backward;
  EXPECT_TRUE(forward.addAlias({.first = SummaryPath::param(0),
                                .second = SummaryPath::param(1),
                                .offset = PointerOffset::ofElements(3)}));
  EXPECT_TRUE(backward.addAlias({.first = SummaryPath::param(1),
                                 .second = SummaryPath::param(0),
                                 .offset = PointerOffset::ofElements(-3)}));
  EXPECT_EQ(forward, backward);
  EXPECT_EQ(forward.aliases.begin()->offset, PointerOffset::ofElements(3));
}

TEST(CallContext, FactsSharesOffsetsAndCallbacksRoundTrip) {
  CallContext input;
  input.callbacks[SummaryPath::param(2).deref().field("callback")] =
      CallTargets::function("a dir/unit.c#drop");
  input.facts[SummaryPath::param(3)] = ValueFact::ofConstant(-42);
  input.facts[SummaryPath::global(0)] = ValueFact::of(Outcome::NonNull);
  EXPECT_TRUE(
      input.addAlias({.first = SummaryPath::param(0),
                      .second = SummaryPath::global(0),
                      .offset = PointerOffset::ofField("struct box .data"),
                      .definite = false,
                      .sameShare = false}));
  const auto text = printCallContext(input, contextGlobalName);
  EXPECT_EQ(parseCallContext(text, resolveContextGlobal), input) << text;
  input.reportDiagnostics = false;
  EXPECT_EQ(parseCallContext(printCallContext(input, contextGlobalName),
                             resolveContextGlobal),
            input);
}

TEST(CallContext, ContradictoryInsertionDoesNotChangeTheContext) {
  auto input = equalInputs();
  const auto before = input;
  EXPECT_FALSE(input.addAlias({.first = SummaryPath::param(0),
                               .second = SummaryPath::param(1),
                               .offset = PointerOffset::ofElements(1)}));
  EXPECT_EQ(input, before);
  EXPECT_TRUE(input.addAlias(*input.aliases.begin()));
  EXPECT_EQ(input, before);
  EXPECT_FALSE(input.addAlias({.first = SummaryPath::result(),
                               .second = SummaryPath::param(2),
                               .offset = {}}));
  EXPECT_EQ(input, before);
}

TEST(CallContext, TransitiveDefiniteOffsetsCannotContradict) {
  auto input = equalInputs();
  EXPECT_TRUE(input.addAlias({.first = SummaryPath::param(1),
                              .second = SummaryPath::param(2),
                              .offset = PointerOffset::ofElements(1)}));
  const auto before = input;
  EXPECT_FALSE(input.addAlias({.first = SummaryPath::param(0),
                               .second = SummaryPath::param(2),
                               .offset = {}}));
  EXPECT_EQ(input, before);
  EXPECT_TRUE(input.addAlias({.first = SummaryPath::param(0),
                              .second = SummaryPath::param(2),
                              .offset = PointerOffset::ofElements(1)}));
}

TEST(CallContext, MayAliasesDoNotCreateTransitiveEqualities) {
  CallContext input;
  EXPECT_TRUE(input.addAlias({.first = SummaryPath::param(0),
                              .second = SummaryPath::param(1),
                              .offset = {},
                              .definite = false}));
  EXPECT_TRUE(input.addAlias({.first = SummaryPath::param(1),
                              .second = SummaryPath::param(2),
                              .offset = {},
                              .definite = false}));
  EXPECT_TRUE(input.addAlias({.first = SummaryPath::param(0),
                              .second = SummaryPath::param(2),
                              .offset = PointerOffset::ofElements(1)}));
  EXPECT_TRUE(input.valid());
}

TEST(CallContext, NullFactsCannotContradictDefiniteEquality) {
  auto input = equalInputs();
  input.facts[SummaryPath::param(0)] = ValueFact::of(Outcome::Null);
  input.facts[SummaryPath::param(1)] = ValueFact::of(Outcome::NonNull);
  EXPECT_FALSE(input.valid());
  EXPECT_FALSE(parseCallContext(printCallContext(input, contextGlobalName),
                                resolveContextGlobal));
}

TEST(CallContext, DistinctSharesAreNotCollapsedByEquality) {
  auto input = equalInputs();
  auto other = *input.aliases.begin();
  other.sameShare = false;
  CallContext distinct;
  EXPECT_TRUE(distinct.addAlias(other));
  EXPECT_NE(input, distinct);
  EXPECT_EQ(parseCallContext(printCallContext(distinct, contextGlobalName),
                             resolveContextGlobal),
            distinct);
}

TEST(CallContext, MissingAnyGlobalPremiseRejectsTheWholeContext) {
  auto input = equalInputs();
  input.facts[SummaryPath::global(0)] = ValueFact::ofConstant(3);
  EXPECT_FALSE(remapCallContext(
      input, [](std::uint32_t) { return std::optional<std::uint32_t>{}; }));
  const auto mapped = remapCallContext(
      input, [](std::uint32_t id) { return std::optional(id + 1); });
  ASSERT_TRUE(mapped);
  EXPECT_EQ(mapped->facts.at(SummaryPath::global(1)), ValueFact::ofConstant(3));
  EXPECT_FALSE(parseCallContext(
      printCallContext(input, contextGlobalName),
      [](std::string_view) { return std::optional<std::uint32_t>{}; }));
}

TEST(CallContext, RenumberingCanReverseACanonicalGlobalPair) {
  CallContext input;
  EXPECT_TRUE(input.addAlias({.first = SummaryPath::global(0),
                              .second = SummaryPath::global(1),
                              .offset = PointerOffset::ofElements(2)}));
  const auto mapped = remapCallContext(
      input, [](std::uint32_t id) { return std::optional(1U - id); });
  ASSERT_TRUE(mapped);
  EXPECT_EQ(mapped->aliases.begin()->offset, PointerOffset::ofElements(-2));
  EXPECT_EQ(
      remapCallContext(*mapped,
                       [](std::uint32_t id) { return std::optional(1U - id); }),
      input);
}

TEST(CallContext, FactOnlyContextsHaveABoundedRoundTrip) {
  CallContext input;
  for (std::uint32_t i = 0; i < MaxCallContextFacts; ++i)
    input.facts[SummaryPath::param(i)] = ValueFact::ofConstant(i);
  ASSERT_TRUE(input.valid());
  EXPECT_EQ(parseCallContext(printCallContext(input, contextGlobalName),
                             resolveContextGlobal),
            input);
  input.facts[SummaryPath::param(MaxCallContextFacts)] =
      ValueFact::ofConstant(1);
  EXPECT_FALSE(input.valid());
}

TEST(CallContext, PointerPathAndDepthBoundsAreEnforced) {
  CallContext input;
  for (std::uint32_t i = 1; i < MaxCallContextPaths; ++i)
    ASSERT_TRUE(input.addAlias({.first = SummaryPath::param(0),
                                .second = SummaryPath::param(i),
                                .offset = {}}));
  EXPECT_FALSE(
      input.addAlias({.first = SummaryPath::param(0),
                      .second = SummaryPath::param(MaxCallContextPaths),
                      .offset = {}}));
  auto path = SummaryPath::param(0);
  for (std::size_t i = 0; i <= MaxHeapPathDepth; ++i)
    path = path.deref();
  input = {};
  input.facts[path] = ValueFact::of(Outcome::Null);
  EXPECT_FALSE(input.valid());
}

TEST(CallContext, EmptyTrivialAndMalformedPathsAreInvalid) {
  EXPECT_FALSE(CallContext{}.valid());
  auto input = equalInputs();
  input.facts[SummaryPath::result()] = ValueFact::ofConstant(1);
  EXPECT_FALSE(input.valid());
  input = equalInputs();
  input.facts[SummaryPath::param(0).field("bad field")] =
      ValueFact::ofConstant(1);
  EXPECT_FALSE(input.valid());
  input = equalInputs();
  auto alias = *input.aliases.begin();
  alias.offset.elements = 4; // noncanonical Zero with an element payload
  input.aliases = {alias};
  EXPECT_FALSE(input.valid());
}

TEST(CallContext, DuplicateAndMalformedRecordsAreRejected) {
  const auto input = equalInputs();
  const auto text = printCallContext(input, contextGlobalName);
  const auto alias = text.substr(text.find(';') + 1);
  for (const auto &bad :
       {std::string{}, text + ';', (text + ';').append(alias), text + ";r:1",
        text + ";v:00:00", text + ";x:0", std::string("r:1"),
        std::string("r:2;") + alias, alias, std::string(262145, 'a')})
    EXPECT_FALSE(parseCallContext(bad, resolveContextGlobal))
        << bad.substr(0, 200);
  for (const auto *bad : {"r:1;a:70:70:30:11", "r:1;v:zz:30",
                          "r:1;v:706172616d20302072656164:3d31",
                          "r:1;v:706172616d2030:0a", "r:1;c:"})
    EXPECT_FALSE(parseCallContext(bad, resolveContextGlobal)) << bad;
}

TEST(CallContext, ExactPathParsingRejectsTrailingOrDeclinedInput) {
  EXPECT_EQ(parseSummaryPath("param 2 *.items[3]", resolveContextGlobal),
            SummaryPath::param(2).deref().field("items").indexed("3"));
  for (const auto *bad : {"param 0 read", "param -1", "param 4294967296",
                          "global missing", "param 0 [nope]", "param 0 ."})
    EXPECT_FALSE(parseSummaryPath(bad, resolveContextGlobal)) << bad;
}

TEST(CallContext, FootprintContainsStoragePrefixesConsumedValuesAndGuards) {
  FunctionSummary summary;
  PlaceEffect effect{.freed = true};
  effect.when.require(SummaryPath::param(0).deref().field("flag"),
                      ValueFact::ofConstant(1));
  summary.addEffect(SummaryPath::param(0).deref().field("data"), effect);
  summary.addEffect(SummaryPath::param(1).deref(), {.written = true});
  const auto footprint = callMemoryFootprint(summary);
  EXPECT_TRUE(footprint.contains(SummaryPath::param(0)));
  EXPECT_TRUE(footprint.contains(SummaryPath::param(0).deref().field("data")));
  EXPECT_TRUE(footprint.contains(SummaryPath::param(0).deref().field("flag")));
  EXPECT_TRUE(footprint.contains(SummaryPath::param(1)));
  EXPECT_FALSE(footprint.contains(SummaryPath::param(1).deref()));
}

TEST(CallContext, ObjectSeparationIsNotJustPointerInequality) {
  CallContext input;
  const auto a = SummaryPath::param(0);
  const auto b = SummaryPath::param(1);
  input.separations.emplace(a, b);
  ASSERT_TRUE(input.valid());
  EXPECT_EQ(parseCallContext(printCallContext(input, contextGlobalName),
                             resolveContextGlobal),
            input);
  EXPECT_FALSE(input.addAlias(
      {.first = a, .second = b, .offset = PointerOffset::ofElements(1)}));
  EXPECT_TRUE(input.aliases.empty());
  input.separations.emplace(b, a);
  EXPECT_FALSE(input.valid());
}

TEST(CallContext, NumericContractsIncludeEveryInputDependency) {
  using Expression = IntegerExpression<SummaryPath>;
  constexpr IntegerType Type{.width = 32, .isSigned = false};
  const auto field = SummaryPath::global(0).deref().field("count");
  const auto result = SummaryPath::param(0).deref();
  const auto argument = SummaryPath::param(1);
  FunctionSummary summary;
  NumericOutput output{.value = Expression::input(field, Type)};
  output.when.requireInteger({.lhs = Expression::input(argument, Type),
                              .op = IntegerOp::Less,
                              .rhs = Expression::input(field, Type)});
  summary.addNumericOutput(result, output);
  summary.addRequirement(
      0, {.need = PathAffine::ofExpression(Expression::input(argument, Type)),
          .start = PathAffine::ofExpression(Expression::input(field, Type))});
  const auto footprint = callMemoryFootprint(summary);
  EXPECT_TRUE(footprint.contains(result));
  EXPECT_TRUE(footprint.contains(SummaryPath::param(0)));
  EXPECT_TRUE(footprint.contains(argument));
  EXPECT_TRUE(footprint.contains(field));
  EXPECT_TRUE(footprint.contains(SummaryPath::global(0)));
}

TEST(CallContext, GlobalSeparationPremisesCannotBeDropped) {
  CallContext input;
  input.separations.emplace(SummaryPath::param(0), SummaryPath::global(0));
  EXPECT_FALSE(remapCallContext(
      input, [](std::uint32_t) { return std::optional<std::uint32_t>{}; }));
  EXPECT_EQ(parseCallContext(printCallContext(input, contextGlobalName),
                             resolveContextGlobal),
            input);
}

TEST(CallContext, ObjectSeparationIntersectsAtJoinsAndDiesWithTheInputValue) {
  AnalysisState left;
  AnalysisState right;
  const PlaceId a{0};
  const PlaceId b{1};
  const PlaceId c{2};
  left.distinctObjects.emplace(a, b);
  left.distinctObjects.emplace(a, c);
  right.distinctObjects.emplace(a, b);
  EXPECT_TRUE(left.join(right));
  EXPECT_EQ(left.distinctObjects, right.distinctObjects);
  left.forget(b);
  EXPECT_TRUE(left.distinctObjects.empty());
  EXPECT_FALSE(right.distinctObjects.empty());
}

TEST(CallContext, TransitiveSharesCannotAlsoBeDistinct) {
  CallContext input;
  const auto a = SummaryPath::param(0);
  const auto b = SummaryPath::param(1);
  const auto c = SummaryPath::param(2);
  ASSERT_TRUE(input.addAlias({.first = a, .second = b, .offset = {}}));
  ASSERT_TRUE(input.addAlias({.first = b, .second = c, .offset = {}}));
  EXPECT_FALSE(input.addAlias(
      {.first = a, .second = c, .offset = {}, .sameShare = false}));
  EXPECT_TRUE(input.valid());
}

TEST(CallContext, GlobalRemappingCannotCollapseTwoPremises) {
  CallContext input;
  ASSERT_TRUE(input.addAlias({.first = SummaryPath::global(0),
                              .second = SummaryPath::global(1),
                              .offset = {}}));
  EXPECT_FALSE(remapCallContext(
      input, [](std::uint32_t) { return std::optional<std::uint32_t>(0); }));
}

} // namespace weavec::core
