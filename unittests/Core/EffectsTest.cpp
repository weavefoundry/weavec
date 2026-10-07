//===- EffectsTest.cpp - Summary joins and renumbering (RFC 0031) ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Effects.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weavec::core {
namespace {

/// A path from its root (`pN` or `gN`) and its steps (`*`, `.name`).
SummaryPath path(std::string_view text) {
  const auto index = static_cast<std::uint32_t>(std::stoul(
      std::string(text.substr(1, text.find_first_not_of("0123456789", 1)))));
  SummaryPath out = text.front() == 'g' ? SummaryPath::global(index)
                                        : SummaryPath::param(index);
  for (std::size_t i = text.find_first_not_of("0123456789", 1);
       i < text.size();) {
    if (text[i] == '*') {
      out = out.deref();
      ++i;
      continue;
    }
    const std::size_t end = text.find_first_of("*.", i + 1);
    out = out.field(text.substr(i + 1, end - i - 1));
    i = end;
  }
  return out;
}

TEST(EffectsTest, JoinKeepsWhatBothDoAndWeakensTheRest) {
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

TEST(EffectsTest, JoinedStoresAgreeOrBecomeUnknown) {
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

TEST(EffectsTest, WideningFoldsWhatAnUnknownEffectCovers) {
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

TEST(EffectsTest, WideningDropsIntegerBoundsThatMove) {
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

TEST(EffectsTest, WideningNumbersNewObjectsByFirstAppearance) {
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

TEST(EffectsTest, RenumberingDropsWhatTheOtherUnitCannotName) {
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
