//===- SummaryIOTest.cpp - Tests for the summary text format --------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/SummaryIO.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace weavec::core {
namespace {

const GlobalNamer Names = [](std::uint32_t id) {
  return id == 0 ? std::string("g_buf") : "g" + std::to_string(id);
};

const GlobalResolver ResolveAll = [](std::string_view name) {
  return std::optional<std::uint32_t>(name == "g_buf" ? 0U : 7U);
};

FunctionSummary sample() {
  FunctionSummary s;
  s.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  s.addEffect(SummaryPath::param(1).deref().field("data"),
              PlaceEffect{.read = true, .written = true});
  s.addEffect(SummaryPath::param(2).deref().field("items").deref(),
              PlaceEffect{.moved = true});
  s.addEffect(SummaryPath::global(0), PlaceEffect{.written = true});
  s.addStore(Store{.dest = SummaryPath::param(1).deref(),
                   .value = ValueSource::fresh()});
  s.addStore(Store{.dest = SummaryPath::global(0),
                   .value = ValueSource::copy(SummaryPath::param(3))});
  s.addReturn(ValueSource::borrow(SummaryPath::param(1).deref().field("v")));
  s.addReturn(ValueSource::null());
  s.addReturn(ValueSource::raw());
  return s;
}

TEST(SummaryIO, PrintsDeterministicRecord) {
  const std::string text = printSummary(sample(), Names);
  EXPECT_EQ(text, "summary\n"
                  "  effect param 0 freed\n"
                  "  effect param 1 *.data read,written\n"
                  "  effect param 2 *.items* moved\n"
                  "  effect global g_buf written\n"
                  "  store param 1 * fresh\n"
                  "  store global g_buf copy param 3\n"
                  "  return borrow param 1 *.v\n"
                  "  return null\n"
                  "  return raw\n"
                  "end\n");
}

TEST(SummaryIO, RoundTrips) {
  const FunctionSummary original = sample();
  std::string error;
  const auto parsed =
      parseSummary(printSummary(original, Names), ResolveAll, &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(*parsed, original);
}

TEST(SummaryIO, RoundTripsEmptyAndReallocLike) {
  FunctionSummary empty;
  EXPECT_EQ(printSummary(empty, Names), "summary\nend\n");
  const auto parsedEmpty = parseSummary("summary\nend\n", ResolveAll);
  ASSERT_TRUE(parsedEmpty);
  EXPECT_EQ(*parsedEmpty, empty);

  FunctionSummary realloc;
  realloc.addEffect(SummaryPath::param(0), PlaceEffect{.moved = true});
  realloc.addReturn(ValueSource::fresh());
  realloc.reallocLike = true;
  const std::string text = printSummary(realloc, Names);
  EXPECT_NE(text.find("  realloc-like\n"), std::string::npos);
  const auto parsed = parseSummary(text, ResolveAll);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, realloc);
}

TEST(SummaryIO, SpellsPathsAndSources) {
  EXPECT_EQ(printSummaryPath(SummaryPath::param(0), Names), "param 0");
  EXPECT_EQ(printSummaryPath(SummaryPath::param(0).indexed(), Names),
            "param 0 []");
  EXPECT_EQ(printSummaryPath(SummaryPath::global(3).deref(), Names),
            "global g3 *");
  EXPECT_EQ(printValueSource(ValueSource::unknown(), Names), "unknown");
  EXPECT_EQ(printValueSource(ValueSource::copy(SummaryPath::global(0)), Names),
            "copy global g_buf");
}

TEST(SummaryIO, DeclinedGlobalsAreDropped) {
  const GlobalResolver declineAll = [](std::string_view) {
    return std::optional<std::uint32_t>();
  };
  const auto parsed = parseSummary(printSummary(sample(), Names), declineAll);
  ASSERT_TRUE(parsed);
  EXPECT_FALSE(parsed->effects.contains(SummaryPath::global(0)));
  EXPECT_TRUE(parsed->effects.contains(SummaryPath::param(0)));
  // The store *into* the global is gone; a store *of* a global would have
  // become unknown.
  EXPECT_EQ(parsed->stores.size(), 1U);
  EXPECT_EQ(parsed->stores.begin()->dest, SummaryPath::param(1).deref());

  FunctionSummary copiesGlobal;
  copiesGlobal.addReturn(ValueSource::copy(SummaryPath::global(0)));
  const auto parsedCopy =
      parseSummary(printSummary(copiesGlobal, Names), declineAll);
  ASSERT_TRUE(parsedCopy);
  EXPECT_TRUE(parsedCopy->returns.contains(ValueSource::unknown()));
  EXPECT_EQ(parsedCopy->returns.size(), 1U);
}

TEST(SummaryIO, RemapGlobalsRenumbersAndDrops) {
  FunctionSummary s;
  s.addEffect(SummaryPath::global(0).deref(), PlaceEffect{.freed = true});
  s.addEffect(SummaryPath::global(1), PlaceEffect{.written = true});
  s.addEffect(SummaryPath::param(0), PlaceEffect{.read = true});
  s.addStore(Store{.dest = SummaryPath::global(1),
                   .value = ValueSource::copy(SummaryPath::global(0))});
  s.addStore(Store{.dest = SummaryPath::param(0).deref(),
                   .value = ValueSource::borrow(SummaryPath::global(1))});
  s.addReturn(ValueSource::copy(SummaryPath::global(0)));
  s.reallocLike = true;

  // 0 -> 5, 1 dropped.
  const FunctionSummary mapped = remapGlobals(s, [](std::uint32_t id) {
    return id == 0 ? std::optional<std::uint32_t>(5) : std::nullopt;
  });
  FunctionSummary expected;
  expected.addEffect(SummaryPath::global(5).deref(),
                     PlaceEffect{.freed = true});
  expected.addEffect(SummaryPath::param(0), PlaceEffect{.read = true});
  expected.addStore(Store{.dest = SummaryPath::param(0).deref(),
                          .value = ValueSource::unknown()});
  expected.addReturn(ValueSource::copy(SummaryPath::global(5)));
  expected.reallocLike = true;
  EXPECT_EQ(mapped, expected);

  const FunctionSummary identity =
      remapGlobals(s, [](std::uint32_t id) { return std::optional(id); });
  EXPECT_EQ(identity, s);
}

TEST(SummaryIO, SkipsUnknownLinesAndBlankLines) {
  const auto parsed = parseSummary("\n\nsummary\n  effect param 0 freed\n"
                                   "  some-future-line with args\n\nend\n\n",
                                   ResolveAll);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->frees(0));
}

TEST(SummaryIO, RejectsMalformedRecords) {
  std::string error;
  EXPECT_FALSE(parseSummary("", ResolveAll, &error));
  EXPECT_EQ(error, "empty record");
  EXPECT_FALSE(parseSummary("summary\n", ResolveAll, &error));
  EXPECT_EQ(error, "missing 'end'");
  EXPECT_FALSE(parseSummary("effect param 0 freed\nend\n", ResolveAll, &error));
  EXPECT_NE(error.find("expected 'summary'"), std::string::npos);
  EXPECT_FALSE(parseSummary("summary\n  effect param x freed\nend\n",
                            ResolveAll, &error));
  EXPECT_NE(error.find("malformed 'effect'"), std::string::npos);
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 flown\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 *.[ freed\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  store param 0 * elsewhere\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(
      parseSummary("summary\n  return copy\nend\n", ResolveAll, &error));
  EXPECT_FALSE(
      parseSummary("summary\n  return null extra\nend\n", ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\nend\ntrailing\n", ResolveAll, &error));
  EXPECT_NE(error.find("after 'end'"), std::string::npos);
}

} // namespace
} // namespace weavec::core
