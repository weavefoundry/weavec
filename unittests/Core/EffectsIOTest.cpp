//===- EffectsIOTest.cpp - Summary format 30 (RFC 0031 §6.1, §7) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/EffectsIO.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace weavec::core {
namespace {

SummaryPath path(std::string_view text) {
  auto parsed = parsePath(text);
  EXPECT_TRUE(parsed) << text;
  return parsed.value_or(SummaryPath{});
}

/// Every field set at least once.
FunctionEffects everything() {
  FunctionEffects effects;
  effects.returns = FunctionEffects::Returns::May;
  effects.incomplete = "the analysis budget was exceeded";
  effects.unknownGlobals = true;
  effects.effects.push_back(PathEffect{
      .kind = PathEffect::Kind::Release,
      .path = path("p0*"),
      .family = "free",
      .when = EffectCase{.classes = {ResultClass::Null},
                         .paramZero = std::make_pair(1U, true)},
      .may = true,
      .lossy = true,
      .offset = 16,
      .elements = ElementRange{
          .from = PathTerm{.path = std::nullopt, .scale = 1, .constant = 0},
          .to = PathTerm{.path = path("p1"), .scale = 8, .constant = -1}}});
  effects.effects.push_back(PathEffect{.kind = PathEffect::Kind::ShareUp,
                                       .path = path("g3*.rc"),
                                       .family = "a family"});
  effects.effects.push_back(PathEffect{
      .kind = PathEffect::Kind::Release,
      .path = path("p0*"),
      .family = "free",
      .when =
          EffectCase{.paramsEqual =
                         ParamPairTest{.first = 0, .second = 2, .equal = true}},
      .may = true,
      .anyOffset = true});
  effects.effects.push_back(PathEffect{
      .kind = PathEffect::Kind::Move,
      .path = path("p1*"),
      .family = "free",
      .when = EffectCase{
          .classes = {ResultClass::Zero},
          .paramZero = std::make_pair(3U, false),
          .paramsEqual = ParamPairTest{.first = 1, .second = 2, .equal = false},
          .entryZero = std::make_pair(path("g2"), true)}});
  effects.stores.push_back(StoreEffect{
      .dest = path("p0*.buf"),
      .value = ValueDesc{.kind = ValueDesc::Kind::Null},
      .when = EffectCase{.entryZero = std::make_pair(path("p0*.buf"), false)}});
  effects.stores.push_back(StoreEffect{
      .dest = path("r*.data[]"),
      .value =
          ValueDesc{.kind = ValueDesc::Kind::Fresh,
                    .family = "free",
                    .extent =
                        PathTerm{.path = path("p1"), .scale = 4, .constant = 2},
                    .zeroed = true,
                    .maybeNull = true,
                    .object = 2,
                    .many = true},
      .when = EffectCase{.classes = {ResultClass::NonNull}},
      .may = true,
      .contents = 1U});
  effects.stores.push_back(StoreEffect{
      .dest = path("p0*.#2.len"),
      .value = ValueDesc{.kind = ValueDesc::Kind::Int, .lo = -3, .hi = 7}});
  effects.stores.push_back(StoreEffect{
      .dest = path("p2*"),
      .value = ValueDesc{.kind = ValueDesc::Kind::Int,
                         .lo = 0,
                         .range = IntegerRange::singleton(IntegerValue::ofBits(
                             *IntegerType::parse("u64"), UINT64_MAX - 1))}});
  effects.stores.push_back(
      StoreEffect{.dest = path("p3*"),
                  .value = ValueDesc{.kind = ValueDesc::Kind::Fresh,
                                     .family = "free",
                                     .maybeNull = true},
                  .absentOn = {ResultClass::Zero, ResultClass::Negative}});
  effects.stores.push_back(
      StoreEffect{.dest = path("p0*.hook"),
                  .value = ValueDesc{.kind = ValueDesc::Kind::Function,
                                     .functions = {"a.c#drop", "free_node"}}});
  effects.stores.push_back(StoreEffect{
      .dest = path("p0*.raw"),
      .value = ValueDesc{
          .kind = ValueDesc::Kind::Unknown, .raw = true, .rawSome = true}});
  // Bytes 24..32 of `*p1` rewritten on some paths.
  effects.stores.push_back(StoreEffect{.dest = path("p1*"),
                                       .value = ValueDesc{},
                                       .may = true,
                                       .bytes = std::make_pair(24, 32)});
  effects.results.push_back(
      ResultEffect{.value = ValueDesc{.kind = ValueDesc::Kind::Path,
                                      .path = path("p0"),
                                      .offset = 8},
                   .classes = {ResultClass::NonNull, ResultClass::Null},
                   .paramZero = std::make_pair(2U, false)});
  effects.results.push_back(ResultEffect{
      .value = ValueDesc{.kind = ValueDesc::Kind::Dangling}, .classes = {}});
  effects.nonNullOn[ResultClass::Zero] = {path("p0"), path("p2")};
  effects.strings.push_back(StringEffect{
      .path = path("r*.data*"),
      .contents = 2U,
      .nulWithin = PathTerm{.path = path("p1"), .scale = 1, .constant = -1},
      .nulFrom = PathTerm{.path = std::nullopt, .scale = 1, .constant = 0}});
  effects.strings.push_back(StringEffect{
      .path = path("p0*"),
      .nulWithin = PathTerm{.path = std::nullopt, .scale = 1, .constant = 5}});
  effects.reads = {path("p0*")};
  effects.writes = {path("g0")};
  return effects;
}

TEST(EffectsIOTest, EveryFieldRoundTrips) {
  FunctionEffects effects = everything();
  std::string text = printEffects(effects);
  std::string error;
  auto parsed = parseEffects(text, &error);
  ASSERT_TRUE(parsed) << error << "\n" << text;
  EXPECT_EQ(*parsed, effects) << text;
  EXPECT_EQ(printEffects(*parsed), text);
}

TEST(EffectsIOTest, AnEmptySummaryRoundTrips) {
  FunctionEffects effects;
  auto parsed = parseEffects(printEffects(effects));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, effects);
}

TEST(EffectsIOTest, PathsSpellTheirSteps) {
  EXPECT_EQ(printPath(path("p0*.next*[i]")), "p0*.next*[i]");
  EXPECT_EQ(path("p0*.next").toString("param"), "param->next");
  // An anonymous member is spelled by its index; other characters are
  // percent-encoded.
  SummaryPath odd = SummaryPath::global(1).field("#1").field("a b");
  EXPECT_EQ(printPath(odd), "g1.#1.a%20b");
  EXPECT_EQ(parsePath(printPath(odd)), odd);
  EXPECT_FALSE(parsePath("q0"));
  EXPECT_FALSE(parsePath("p"));
  EXPECT_FALSE(parsePath("p0["));
}

TEST(EffectsIOTest, MalformedTextNamesTheLine) {
  std::string error;
  EXPECT_FALSE(
      parseEffects("returns always\neffect release p0* when=x", &error));
  EXPECT_NE(error.find("line 2"), std::string::npos) << error;
  EXPECT_FALSE(parseEffects("effect release p0* when=-:-", &error));
  EXPECT_NE(error.find("no returns"), std::string::npos) << error;
  EXPECT_FALSE(parseEffects("returns always\nstore p0* when=-:- :: bogus"));
  EXPECT_FALSE(parseEffects("returns sometimes"));
}

TEST(EffectsIOTest, JoinKeepsWhatBothDoAndWeakensTheRest) {
  FunctionEffects left;
  left.effects.push_back(
      PathEffect{.kind = PathEffect::Kind::Release, .path = path("p0*")});
  left.effects.push_back(
      PathEffect{.kind = PathEffect::Kind::Release, .path = path("p1*")});
  left.results.push_back(ResultEffect{
      .value = ValueDesc{.kind = ValueDesc::Kind::Fresh, .object = 0},
      .classes = {ResultClass::NonNull}});
  left.nonNullOn[ResultClass::Zero] = {path("p0"), path("p1")};
  FunctionEffects right;
  right.returns = FunctionEffects::Returns::Never;
  right.effects.push_back(
      PathEffect{.kind = PathEffect::Kind::Release, .path = path("p0*")});
  right.results.push_back(ResultEffect{
      .value = ValueDesc{.kind = ValueDesc::Kind::Fresh, .object = 0},
      .classes = {ResultClass::NonNull}});
  right.nonNullOn[ResultClass::Zero] = {path("p1")};

  FunctionEffects joined = joinEffects(left, right);
  EXPECT_EQ(joined.returns, FunctionEffects::Returns::May);
  ASSERT_EQ(joined.effects.size(), 2U);
  EXPECT_FALSE(joined.effects[0].may); // both release *p0
  EXPECT_TRUE(joined.effects[1].may);  // only the left releases *p1
  // One new object: the two sides' differ only by their number, and no
  // store names either (a widening join would otherwise add one per round).
  ASSERT_EQ(joined.results.size(), 1U);
  // Named by a store, they stay two.
  right.stores.push_back(StoreEffect{
      .dest = path("p1*"),
      .value = ValueDesc{.kind = ValueDesc::Kind::Fresh, .object = 0}});
  FunctionEffects named = joinEffects(left, right);
  ASSERT_EQ(named.results.size(), 2U);
  EXPECT_NE(named.results[0].value.object, named.results[1].value.object);
  EXPECT_EQ(joined.nonNullOn[ResultClass::Zero],
            std::vector<SummaryPath>{path("p1")});
}

TEST(EffectsIOTest, JoinedStoresAgreeOrBecomeUnknown) {
  FunctionEffects left;
  left.stores.push_back(StoreEffect{
      .dest = path("p0*.n"),
      .value = ValueDesc{.kind = ValueDesc::Kind::Int, .lo = 1, .hi = 1}});
  FunctionEffects right = left;
  right.stores[0].value.hi = 2;
  FunctionEffects joined = joinEffects(left, right);
  ASSERT_EQ(joined.stores.size(), 1U);
  EXPECT_EQ(joined.stores[0].value.kind, ValueDesc::Kind::Unknown);
  EXPECT_FALSE(joined.stores[0].may);
  FunctionEffects same = joinEffects(left, left);
  EXPECT_EQ(same.stores[0].value.hi, 1);
}

TEST(EffectsIOTest, WideningFoldsWhatAnUnknownEffectCovers) {
  // Round after round a recursion over a tree names a deeper path; below
  // an unknown effect on every case, it says nothing the caller does not
  // already forget.
  FunctionEffects previous;
  previous.effects.push_back(PathEffect{
      .kind = PathEffect::Kind::Unknown, .path = path("p0*"), .may = true});
  previous.stores.push_back(
      StoreEffect{.dest = path("p0*.left"),
                  .value = ValueDesc{.kind = ValueDesc::Kind::Null}});
  FunctionEffects next = previous;
  next.effects.push_back(PathEffect{.kind = PathEffect::Kind::Unknown,
                                    .path = path("p0*.left*"),
                                    .may = true});
  next.effects.push_back(PathEffect{.kind = PathEffect::Kind::Release,
                                    .path = path("p0*.right*"),
                                    .family = "free",
                                    .may = true});
  FunctionEffects widened = widenEffects(previous, next);
  ASSERT_EQ(widened.effects.size(), 1U);
  EXPECT_EQ(widened.effects[0].path, path("p0*"));
  EXPECT_TRUE(widened.stores.empty());
  EXPECT_TRUE(widenEffects(widened, next) == widened);
}

TEST(EffectsIOTest, WideningDropsIntegerBoundsThatMove) {
  FunctionEffects previous;
  previous.results.push_back(ResultEffect{
      .value = ValueDesc{.kind = ValueDesc::Kind::Int, .lo = 0, .hi = 1},
      .classes = {ResultClass::Zero, ResultClass::Positive}});
  FunctionEffects next;
  next.results.push_back(ResultEffect{
      .value = ValueDesc{.kind = ValueDesc::Kind::Int, .lo = 0, .hi = 2},
      .classes = {ResultClass::Zero, ResultClass::Positive}});
  FunctionEffects widened = widenEffects(previous, next);
  ASSERT_EQ(widened.results.size(), 1U);
  EXPECT_EQ(widened.results[0].value.lo, 0);
  EXPECT_FALSE(widened.results[0].value.hi);
  EXPECT_TRUE(widenEffects(widened, next) == widened);
}

TEST(EffectsIOTest, WideningNumbersNewObjectsByFirstAppearance) {
  // The join numbers the right side's objects after the left's; the same
  // allocation from two rounds compares equal once renumbered.
  FunctionEffects round;
  round.results.push_back(ResultEffect{
      .value = ValueDesc{.kind = ValueDesc::Kind::Fresh, .object = 0},
      .classes = {ResultClass::NonNull}});
  FunctionEffects widened = widenEffects(round, round);
  ASSERT_EQ(widened.results.size(), 1U);
  EXPECT_EQ(widened.results[0].value.object, 0U);
  EXPECT_TRUE(widenEffects(widened, round) == widened);
}

TEST(EffectsIOTest, RenumberingDropsWhatTheOtherUnitCannotName) {
  FunctionEffects effects;
  effects.effects.push_back(
      PathEffect{.kind = PathEffect::Kind::Release, .path = path("g0*")});
  effects.effects.push_back(
      PathEffect{.kind = PathEffect::Kind::Release, .path = path("g1*")});
  effects.stores.push_back(StoreEffect{
      .dest = path("g1"), .value = ValueDesc{.kind = ValueDesc::Kind::Null}});
  effects.stores.push_back(StoreEffect{
      .dest = path("p0*"),
      .value = ValueDesc{.kind = ValueDesc::Kind::Path, .path = path("g1")}});
  auto map = [](std::uint32_t id) -> std::optional<std::uint32_t> {
    if (id == 0)
      return 5;
    return std::nullopt;
  };
  FunctionEffects out = renumberGlobals(effects, map);
  ASSERT_EQ(out.effects.size(), 1U);
  EXPECT_EQ(out.effects[0].path, path("g5*"));
  ASSERT_TRUE(out.incomplete);
  // (The store into `g1` still writes a global.)
  EXPECT_TRUE(out.unknownGlobals);
  ASSERT_EQ(out.stores.size(), 1U);
  EXPECT_EQ(out.stores[0].value.kind, ValueDesc::Kind::Unknown);
}

} // namespace
} // namespace weavec::core
