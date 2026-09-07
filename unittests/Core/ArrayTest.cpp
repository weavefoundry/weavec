//===- ArrayTest.cpp - RFC 0015 selections and range algebra --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Array.h"

#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/Place.h"
#include "weavec/Core/SummaryIO.h"

#include <gtest/gtest.h>

#include <limits>

namespace weavec::core {

TEST(Array, SelectorsRoundTripWithoutConfusingConstantsAndSymbols) {
  for (const auto &text :
       {"0", "-4", "9223372036854775807", "-9223372036854775808", "$0", "$2+3",
        "$2-3", "$4294967295-9223372036854775808"}) {
    const auto index = ArrayIndex::parse(text);
    ASSERT_TRUE(index) << text;
    EXPECT_EQ(index->toString(), text);
  }
  EXPECT_NE(ArrayIndex::constant(2), ArrayIndex::variable(2));
}

TEST(Array, MalformedAndOverflowingSelectorsAreRejected) {
  for (const auto &text : {"", "$", "$-1", "$4294967296", "$2+", "$2+-1", "$2x",
                           "9223372036854775808", "1.2", "3junk"})
    EXPECT_FALSE(ArrayIndex::parse(text)) << text;
  EXPECT_FALSE(ArrayIndex::constant(INT64_MAX).shifted(1));
  EXPECT_FALSE(ArrayIndex::constant(INT64_MIN).shifted(-1));
  EXPECT_FALSE(
      ArrayIndex::constant(INT64_MAX).difference(ArrayIndex::constant(-1)));
  EXPECT_FALSE(ArrayIndex::variable(0).difference(ArrayIndex::variable(1)));
}

TEST(Array, HalfOpenIntervalsDistinguishDisjointAndUnknown) {
  const ArrayInterval range{.begin = ArrayIndex::variable(0),
                            .end = ArrayIndex::variable(0, 4)};
  EXPECT_EQ(range.length(), 4);
  EXPECT_EQ(range.contains(ArrayIndex::variable(0)), ArrayRelation::Yes);
  EXPECT_EQ(range.contains(ArrayIndex::variable(0, 3)), ArrayRelation::Yes);
  EXPECT_EQ(range.contains(ArrayIndex::variable(0, 4)), ArrayRelation::No);
  EXPECT_EQ(range.contains(ArrayIndex::variable(0, -1)), ArrayRelation::No);
  EXPECT_EQ(range.contains(ArrayIndex::variable(1)), ArrayRelation::Unknown);
  EXPECT_EQ(range.overlaps(*range.shifted(3)), ArrayRelation::Yes);
  EXPECT_EQ(range.overlaps(*range.shifted(4)), ArrayRelation::No);
  EXPECT_EQ(
      range.overlaps({ArrayIndex::variable(1), ArrayIndex::variable(1, 4)}),
      ArrayRelation::Unknown);
  const ArrayInterval empty{.begin = ArrayIndex::constant(2),
                            .end = ArrayIndex::constant(2)};
  EXPECT_EQ(empty.contains(ArrayIndex::variable(3)), ArrayRelation::No);
  EXPECT_EQ(empty.overlaps(range), ArrayRelation::No);
}

TEST(Array, SelectedPlacesRetainNestedDimensionsAndTranslations) {
  PlaceTable places;
  const auto a = places.create("a");
  const auto b = places.create("b");
  const auto zero = places.element(places.index(a), "0");
  const auto one = places.element(places.index(a), "1");
  EXPECT_NE(zero, one);
  EXPECT_EQ(places.element(places.index(a), "0"), zero);
  const auto nested = places.element(places.index(zero), "1");
  EXPECT_EQ(places.name(nested), "a[0][1]");
  const auto translated = places.translate(nested, a, b);
  EXPECT_EQ(places.name(translated), "b[0][1]");
  EXPECT_EQ(places.lookupTranslated(nested, a, b), translated);
  EXPECT_TRUE(places.isElement(zero));
  EXPECT_FALSE(places.isElement(places.index(a)));
}

TEST(Array, SelectedSummaryPathsRoundTripAndRemap) {
  FunctionSummary original;
  const auto path = SummaryPath::global(3).indexed().indexed("$1+2").deref();
  original.addEffect(path, PlaceEffect{.read = true});
  original.addStore({.dest = SummaryPath::param(0).deref().indexed("1"),
                     .value = ValueSource::copy(path)});
  const auto text =
      printSummary(original, [](std::uint32_t) { return "table"; });
  const auto parsed = parseSummary(text, [](std::string_view) { return 3U; });
  ASSERT_TRUE(parsed) << text;
  EXPECT_EQ(*parsed, original);
  const auto remapped =
      remapGlobals(original, [](std::uint32_t) { return 7U; });
  EXPECT_TRUE(remapped.effects.contains(
      SummaryPath::global(7).indexed().indexed("$1+2").deref()));
  EXPECT_EQ(remapped.stores.begin()->value.path->index, 7U);
}

TEST(Array, SymbolicMembershipRequiresActualOrderEvidence) {
  const PlaceId n{0};
  const PlaceId i{1};
  ScalarTracker scalars;
  RelationTracker relations;
  const ArraySpan span{.begin = ArrayIndex::constant(0),
                       .count = Affine::ofPlace(n)};
  EXPECT_EQ(span.contains(ArrayIndex::constant(0), scalars, relations),
            ArrayRelation::Unknown);
  scalars.set(n, ValueFact::of(Outcome::Positive));
  EXPECT_EQ(span.contains(ArrayIndex::constant(0), scalars, relations),
            ArrayRelation::Yes);
  EXPECT_EQ(span.contains(ArrayIndex::constant(1), scalars, relations),
            ArrayRelation::Unknown);
  relations.learn(i, Relation::Less, n);
  EXPECT_EQ(span.contains(ArrayIndex::variable(i.value), scalars, relations),
            ArrayRelation::Unknown);
  relations.learnAtLeast(i, 0);
  EXPECT_EQ(span.contains(ArrayIndex::variable(i.value), scalars, relations),
            ArrayRelation::Yes);
  EXPECT_EQ(span.contains(ArrayIndex::variable(n.value), scalars, relations),
            ArrayRelation::No);
}

TEST(Array, RangeSummariesRoundTripAndValidateTheirBounds) {
  FunctionSummary original;
  original.arrayCopies.insert(
      {.dest = SummaryPath::param(0).deref(),
       .source = SummaryPath::global(2).indexed(),
       .destBegin = PathAffine::ofConstant(0),
       .sourceBegin = PathAffine::ofConstant(1),
       .count = PathAffine::ofPath(SummaryPath::param(1)),
       .elementBytes = 8,
       .view = {},
       .when = {},
       .definite = true});
  const auto text =
      printSummary(original, [](std::uint32_t) { return "table"; });
  const auto parsed = parseSummary(text, [](std::string_view) { return 2U; });
  ASSERT_TRUE(parsed) << text;
  EXPECT_EQ(*parsed, original);
  const auto mapped = remapGlobals(original, [](std::uint32_t) { return 3U; });
  EXPECT_EQ(mapped.arrayCopies.begin()->source.index, 3U);
  for (const auto &suffix :
       {"count -1 bytes 8", "count 2 bytes 0", "count 2 bytes -1"}) {
    const auto invalid = std::string("summary\narray-copy param 0 * from param "
                                     "1 * dest-begin 0 source-begin 0 ") +
                         suffix + " view pointer definite\nend\n";
    EXPECT_FALSE(parseSummary(invalid, {})) << invalid;
  }
}

TEST(Array, DisjointnessNeedsAProofAndHandlesOffsets) {
  ScalarTracker scalars;
  RelationTracker relations;
  const auto i = ArrayIndex::variable(0);
  const auto j = ArrayIndex::variable(1);
  EXPECT_FALSE(arrayIndicesDisjoint(i, j, scalars, relations));
  EXPECT_TRUE(arrayIndicesDisjoint(i, *i.shifted(1), scalars, relations));
  relations.learn(PlaceId{0}, Relation::Less, PlaceId{1});
  EXPECT_TRUE(arrayIndicesDisjoint(i, j, scalars, relations));
  EXPECT_FALSE(arrayIndicesDisjoint(*i.shifted(1), j, scalars, relations));
  relations.learn(PlaceId{0}, Relation::Equal, PlaceId{1}, 2);
  EXPECT_TRUE(arrayIndicesDisjoint(i, j, scalars, relations));
  EXPECT_FALSE(arrayIndicesDisjoint(i, *j.shifted(2), scalars, relations));
}

TEST(Array, RangeJoinsWeakenMustFactsWithoutDeletingTemporalEvidence) {
  PlaceTable places;
  const auto a = places.create("a");
  const auto b = places.create("b");
  const auto snapshot = places.create("input");
  const auto first = places.element(a, "0");
  const auto second = places.element(a, "1");
  AnalysisState left;
  AnalysisState right;
  left.arrayRanges[snapshot] = {.destination = a,
                                .source = b,
                                .snapshot = snapshot,
                                .span = {.begin = ArrayIndex::constant(0),
                                         .count = Affine::ofConstant(3)},
                                .sourceBegin = ArrayIndex::constant(0),
                                .captured = {ArrayIndex::constant(0)},
                                .materialized = {ArrayIndex::constant(0)},
                                .exported = std::nullopt};
  left.moves.markMoved(first, MoveReason::Freed, {});
  right.moves.markMoved(second, MoveReason::Freed, {});
  EXPECT_TRUE(left.join(right, &places));
  EXPECT_FALSE(left.arrayRanges.at(snapshot).definite);
  EXPECT_TRUE(left.arrayRanges.at(snapshot).captured.empty());
  EXPECT_TRUE(left.incompleteHeap.contains(a));
  EXPECT_TRUE(left.moves.recordOf(first));
  EXPECT_TRUE(left.moves.recordOf(second));
  EXPECT_FALSE(left.join(right, &places));
}

TEST(Array, TraversalsRoundTripAndRemapEveryInterfaceDependency) {
  FunctionSummary original;
  PathGuard when;
  when.require(SummaryPath::global(1), ValueFact::of(Outcome::Positive));
  original.arrayFills.insert(
      {.storage = SummaryPath::global(0).indexed(),
       .count = PathAffine::ofPath(SummaryPath::global(1)),
       .bytes = 16,
       .when = when});
  original.arrayReleases.insert(
      {.storage = SummaryPath::param(0).deref(),
       .begin = PathAffine::ofConstant(0),
       .count = PathAffine::ofPath(SummaryPath::global(1)),
       .when = when,
       .cleared = true});
  const auto name = [](std::uint32_t id) { return std::to_string(id); };
  const auto resolve = [](std::string_view id) {
    return static_cast<std::uint32_t>(id.front() - '0');
  };
  const auto text = printSummary(original, name);
  const auto parsed = parseSummary(text, resolve);
  ASSERT_TRUE(parsed) << text;
  EXPECT_EQ(*parsed, original);
  EXPECT_EQ(printSummary(*parsed, name), text);
  const auto mapped =
      remapGlobals(original, [](std::uint32_t id) { return id + 2; });
  EXPECT_EQ(mapped.arrayFills.begin()->storage.index, 2U);
  EXPECT_EQ(mapped.arrayFills.begin()->count.path->index, 3U);
  EXPECT_EQ(mapped.arrayReleases.begin()->when.conditions.begin()->first.index,
            3U);
  EXPECT_FALSE(original.empty());
  FunctionSummary joined;
  joined.join(original);
  EXPECT_EQ(joined, original);
  joined.join(original);
  EXPECT_EQ(joined, original);
}

TEST(Array, InvalidTraversalAndRangeInterfacesAreRejected) {
  for (const auto &line :
       {"array-fill param 0 count 3 malloc 4 definite",
        "array-fill param 0 * count -1 malloc 4 definite",
        "array-fill param 0 * count 3 malloc -1 definite",
        "array-fill param 0 * count 3 mystery definite",
        "array-release result * begin 0 count 3 retained definite",
        "array-release param 0 * begin 0 count -1 cleared definite",
        "array-copy param 0 * from result * dest-begin 0 source-begin 0 count "
        "3 bytes 8 view pointer definite",
        "array-copy param 0 *[$-1] from param 1 * dest-begin 0 source-begin 0 "
        "count 3 bytes 8 view pointer definite"})
    EXPECT_FALSE(parseSummary(std::string("summary\n") + line + "\nend\n", {}))
        << line;
  std::string oversized = "summary\n";
  for (std::size_t i = 0; i <= MaxArrayRanges; ++i)
    oversized += "array-release param 0 * begin " + std::to_string(i) +
                 " count 1 retained definite\n";
  oversized += "end\n";
  EXPECT_FALSE(parseSummary(oversized, {}));
}

TEST(Array, TraversalJoinsAndLostGuardsCannotInventDefiniteWrites) {
  FunctionSummary original;
  PathGuard guard;
  guard.require(SummaryPath::global(4), ValueFact::of(Outcome::Positive));
  original.arrayFills.insert({.storage = SummaryPath::param(0).deref(),
                              .count = PathAffine::ofConstant(2),
                              .bytes = std::nullopt,
                              .when = guard});
  original.arrayReleases.insert({.storage = SummaryPath::param(0).deref(),
                                 .begin = PathAffine::ofConstant(0),
                                 .count = PathAffine::ofConstant(2),
                                 .when = guard,
                                 .cleared = true});
  auto joined = original;
  FunctionSummary untouched;
  untouched.addEffect(SummaryPath::param(0), PlaceEffect{.read = true});
  joined.join(untouched);
  EXPECT_FALSE(joined.arrayFills.begin()->definite);
  EXPECT_FALSE(joined.arrayReleases.begin()->definite);
  const auto mapped =
      remapGlobals(original, [](std::uint32_t) -> std::optional<std::uint32_t> {
        return std::nullopt;
      });
  EXPECT_FALSE(mapped.arrayFills.begin()->definite);
  EXPECT_FALSE(mapped.arrayReleases.begin()->definite);
  EXPECT_TRUE(mapped.incomplete.contains(
      "array range guard lost in program interface"));
  EXPECT_TRUE(mapped.arrayFills.begin()->when.trivial());
  EXPECT_TRUE(parseSummary(printSummary(mapped, {}), {}));
}

} // namespace weavec::core
