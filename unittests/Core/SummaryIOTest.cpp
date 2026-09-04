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

TEST(SummaryIO, RoundTripsEmpty) {
  FunctionSummary empty;
  EXPECT_EQ(printSummary(empty, Names), "summary\nend\n");
  const auto parsedEmpty = parseSummary("summary\nend\n", ResolveAll);
  ASSERT_TRUE(parsedEmpty);
  EXPECT_EQ(*parsedEmpty, empty);
}

// RFC 0006, *Text format*: `realloc` is spelled with outcome lines.
TEST(SummaryIO, PrintsAndParsesOutcomes) {
  FunctionSummary realloc;
  realloc.addEffect(SummaryPath::param(0), PlaceEffect{.moved = true});
  realloc.addReturn(ValueSource::fresh());
  realloc.addReturn(ValueSource::null());
  realloc.addOutcome(Outcome::Null);
  realloc.addOutcome(Outcome::NonNull, SummaryPath::param(0),
                     PlaceEffect{.moved = true});
  const std::string text = printSummary(realloc, Names);
  EXPECT_EQ(text, "summary\n"
                  "  effect param 0 moved\n"
                  "  return fresh\n"
                  "  return null\n"
                  "  outcome null\n"
                  "  outcome nonnull param 0 moved\n"
                  "end\n");
  const auto parsed = parseSummary(text, ResolveAll);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, realloc);

  FunctionSummary status;
  status.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  status.addOutcome(Outcome::Zero, SummaryPath::param(0),
                    PlaceEffect{.freed = true});
  status.addOutcome(Outcome::Negative);
  status.addOutcome(Outcome::Positive, SummaryPath::param(1).deref(),
                    PlaceEffect{.freed = true, .moved = true});
  const auto reparsed = parseSummary(printSummary(status, Names), ResolveAll);
  ASSERT_TRUE(reparsed);
  EXPECT_EQ(*reparsed, status);

  std::string error;
  EXPECT_FALSE(
      parseSummary("summary\n  outcome maybe\nend\n", ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  outcome zero param 0\nend\n",
                            ResolveAll, &error));
}

// RFC 0009, *Summary text format (version 5)*: `when` guards and
// `never-returns`.
TEST(SummaryIO, PrintsAndParsesGuardsAndNeverReturns) {
  const auto when =
      [](std::initializer_list<std::pair<SummaryPath, ValueFact>> conjuncts) {
        PathGuard guard;
        for (const auto &[path, fact] : conjuncts)
          guard.require(path, fact);
        return guard;
      };
  FunctionSummary alloc;
  alloc.addEffect(SummaryPath::param(1),
                  PlaceEffect{.freed = true,
                              .family = "free",
                              .when = when({{SummaryPath::param(3),
                                             ValueFact::of(Outcome::Zero)}})});
  alloc.addOutcome(Outcome::Null, SummaryPath::param(1),
                   PlaceEffect{.freed = true,
                               .family = "free",
                               .when = when({{SummaryPath::param(3),
                                              ValueFact::of(Outcome::Zero)}})});
  ValueSource fresh = ValueSource::fresh("free");
  fresh.when = when({{SummaryPath::param(3), ValueFact::nonZero()}});
  alloc.addReturn(fresh);
  alloc.addReturn(ValueSource::null());
  ValueSource copy = ValueSource::copy(SummaryPath::param(2));
  copy.when = when(
      {{SummaryPath::param(2), ValueFact::of(Outcome::NonNull)},
       {SummaryPath::param(0).deref().field("n"), ValueFact::ofConstant(-3)}});
  alloc.addStore(
      Store{.dest = SummaryPath::param(0).deref().field("msg"), .value = copy});

  const std::string text = printSummary(alloc, Names);
  EXPECT_EQ(text, "summary\n"
                  "  effect param 1 freed(free) when param 3 zero\n"
                  "  store param 0 *.msg copy param 2 when param 0 *.n =-3 "
                  "and param 2 nonnull\n"
                  "  return fresh(free) when param 3 positive|negative\n"
                  "  return null\n"
                  "  outcome null param 1 freed(free) when param 3 zero\n"
                  "end\n");
  std::string error;
  const auto parsed = parseSummary(text, ResolveAll, &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(*parsed, alloc);

  FunctionSummary dies;
  dies.neverReturns = true;
  dies.addEffect(SummaryPath::param(0).deref(), PlaceEffect{.read = true});
  EXPECT_EQ(printSummary(dies, Names), "summary\n"
                                       "  never-returns\n"
                                       "  effect param 0 * read\n"
                                       "end\n");
  const auto reparsed = parseSummary(printSummary(dies, Names), ResolveAll);
  ASSERT_TRUE(reparsed);
  EXPECT_EQ(*reparsed, dies);
  EXPECT_TRUE(reparsed->neverReturns);
}

TEST(SummaryIO, GuardsOnDeclinedGlobalsAreDropped) {
  const GlobalResolver declineAll = [](std::string_view) {
    return std::optional<std::uint32_t>();
  };
  const auto parsed =
      parseSummary("summary\n"
                   "  effect param 0 freed when global g1 zero and param 1 "
                   "positive\n"
                   "end\n",
                   declineAll);
  ASSERT_TRUE(parsed);
  PathGuard expected;
  expected.require(SummaryPath::param(1), ValueFact::of(Outcome::Positive));
  EXPECT_EQ(parsed->effectOf(SummaryPath::param(0)).when, expected);
}

TEST(SummaryIO, RejectsMalformedGuards) {
  for (const char *line : {
           "  effect param 0 freed when",
           "  effect param 0 freed when param 3",
           "  effect param 0 freed when param 3 maybe",
           "  effect param 0 freed when param 3 zero param 2 zero",
           "  effect param 0 freed when param 3 zero and",
           "  effect param 0 freed if param 3 zero",
           "  effect param 0 read when param 3 zero",
           "  return null when param 1",
           "  store param 0 * fresh when =3",
       }) {
    std::string error;
    EXPECT_FALSE(parseSummary(std::string("summary\n") + line + "\nend\n",
                              ResolveAll, &error))
        << line;
    EXPECT_FALSE(error.empty()) << line;
  }
}

TEST(SummaryIO, InteriorCopiesRoundTrip) {
  FunctionSummary s;
  s.addReturn(ValueSource::interiorCopy(SummaryPath::param(0)));
  s.addStore(Store{.dest = SummaryPath::param(1).deref(),
                   .value = ValueSource::copy(SummaryPath::param(0))});
  const std::string text = printSummary(s, Names);
  EXPECT_NE(text.find("  return interior param 0\n"), std::string::npos);
  EXPECT_NE(text.find("  store param 1 * copy param 0\n"), std::string::npos);
  const auto parsed = parseSummary(text, ResolveAll);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, s);
  EXPECT_NE(ValueSource::interiorCopy(SummaryPath::param(0)),
            ValueSource::copy(SummaryPath::param(0)));
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
  EXPECT_EQ(
      printValueSource(ValueSource::interiorCopy(SummaryPath::param(2)), Names),
      "interior param 2");
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
  s.addOutcome(Outcome::Zero, SummaryPath::global(0).deref(),
               PlaceEffect{.freed = true});
  s.addOutcome(Outcome::Zero, SummaryPath::global(1),
               PlaceEffect{.moved = true});
  s.addOutcome(Outcome::Positive);

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
  expected.addOutcome(Outcome::Zero, SummaryPath::global(5).deref(),
                      PlaceEffect{.freed = true});
  expected.addOutcome(Outcome::Positive);
  EXPECT_EQ(mapped, expected);

  const FunctionSummary identity =
      remapGlobals(s, [](std::uint32_t id) { return std::optional(id); });
  EXPECT_EQ(identity, s);
}

// RFC 0007, *Summary text format*: `freed`, `moved` and `fresh` carry their
// release family in parentheses, and `null <class> <path>` records a caller
// place that is null in an outcome class.
TEST(SummaryIO, PrintsAndParsesFamiliesAndNullOn) {
  FunctionSummary s;
  s.addEffect(SummaryPath::param(0),
              PlaceEffect{.freed = true, .family = "fclose"});
  s.addEffect(SummaryPath::param(1),
              PlaceEffect{.read = true, .moved = true, .family = "free"});
  s.addStore(Store{.dest = SummaryPath::param(2).deref(),
                   .value = ValueSource::fresh("free")});
  s.addReturn(ValueSource::fresh("closedir"));
  // A class with null facts is a possible class (RFC 0006).
  s.addOutcome(Outcome::Zero);
  s.addOutcome(Outcome::Negative);
  s.nullOn[Outcome::Negative].insert(SummaryPath::param(2).deref());
  s.nullOn[Outcome::Negative].insert(SummaryPath::param(3).deref());
  s.nullOn[Outcome::Zero].insert(SummaryPath::param(2).deref());

  const std::string text = printSummary(s, Names);
  EXPECT_EQ(text, "summary\n"
                  "  effect param 0 freed(fclose)\n"
                  "  effect param 1 read,moved(free)\n"
                  "  store param 2 * fresh(free)\n"
                  "  return fresh(closedir)\n"
                  "  outcome zero\n"
                  "  outcome negative\n"
                  "  null zero param 2 *\n"
                  "  null negative param 2 *\n"
                  "  null negative param 3 *\n"
                  "end\n");
  std::string error;
  const auto parsed = parseSummary(text, ResolveAll, &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(*parsed, s);
  EXPECT_EQ(parsed->freshReturnFamily(), "closedir");
}

TEST(SummaryIO, BareFlagsHaveNoFamily) {
  // A version 2 record (no families) still parses; the family is unknown.
  const auto parsed = parseSummary("summary\n  effect param 0 freed\n"
                                   "  return fresh\nend\n",
                                   ResolveAll);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->effectOf(SummaryPath::param(0)).family, "");
  EXPECT_EQ(parsed->freshReturnFamily(), "");
}

TEST(SummaryIO, RejectsMalformedFamilies) {
  std::string error;
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 freed(\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 freed()\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 freed(free)x\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 read(free)\nend\n",
                            ResolveAll, &error))
      << "only freed and moved take a family";
  EXPECT_FALSE(
      parseSummary("summary\n  return null(free)\nend\n", ResolveAll, &error))
      << "only fresh takes a family";
  EXPECT_FALSE(parseSummary("summary\n  null bogus param 0 *\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  null zero\nend\n", ResolveAll, &error));
}

TEST(SummaryIO, SkipsUnknownLinesAndBlankLines) {
  const auto parsed = parseSummary("\n\nsummary\n  effect param 0 freed\n"
                                   "  some-future-line with args\n\nend\n\n",
                                   ResolveAll);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->frees(0));
}

// RFC 0008, *Summary text format* (version 4): the `replaced` flag, the
// `result` root, `notnull <class> <path>` and `requires <index>`.
TEST(SummaryIO, PrintsAndParsesPointerValidityFacts) {
  FunctionSummary s;
  s.addEffect(
      SummaryPath::param(0).deref().field("buf"),
      PlaceEffect{
          .written = true, .freed = true, .replaced = true, .family = "free"});
  s.addEffect(SummaryPath::global(0).deref(),
              PlaceEffect{.freed = true, .element = true, .family = "free"});
  s.addStore(Store{.dest = SummaryPath::param(0).deref().field("buf"),
                   .value = ValueSource::fresh("free")});
  s.addStore(Store{.dest = SummaryPath::result().field("name"),
                   .value = ValueSource::copy(SummaryPath::param(1))});
  s.addOutcome(Outcome::Zero);
  s.addOutcome(Outcome::Negative);
  s.nonNullOn[Outcome::Zero].insert(SummaryPath::param(2).deref());
  s.requiresNonNull.insert(0);
  s.requiresNonNull.insert(2);

  const std::string text = printSummary(s, Names);
  EXPECT_EQ(text, "summary\n"
                  "  effect param 0 *.buf written,freed(free),replaced\n"
                  "  effect global g_buf * freed(free),element\n"
                  "  store param 0 *.buf fresh(free)\n"
                  "  store result .name copy param 1\n"
                  "  outcome zero\n"
                  "  outcome negative\n"
                  "  notnull zero param 2 *\n"
                  "  requires 0\n"
                  "  requires 2\n"
                  "end\n");
  std::string error;
  const auto parsed = parseSummary(text, ResolveAll, &error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(*parsed, s);
  EXPECT_TRUE(parsed->requiresParam(0));
  EXPECT_FALSE(parsed->requiresParam(1));
  EXPECT_TRUE(parsed->requiresParam(2));
  EXPECT_TRUE(
      parsed->effectOf(SummaryPath::param(0).deref().field("buf")).replaced);
  EXPECT_TRUE(parsed->effectOf(SummaryPath::global(0).deref()).element);
  EXPECT_TRUE(parsed->storesTo(SummaryPath::result().field("name")));
}

TEST(SummaryIO, SpellsTheResultRoot) {
  EXPECT_EQ(printSummaryPath(SummaryPath::result(), Names), "result");
  EXPECT_EQ(printSummaryPath(SummaryPath::result().field("p").deref(), Names),
            "result .p*");
  EXPECT_TRUE(SummaryPath::result().isResult());
  EXPECT_FALSE(SummaryPath::result().isParam());
  EXPECT_FALSE(SummaryPath::result().isGlobal());
  EXPECT_FALSE(SummaryPath::param(0).isResult());
}

TEST(SummaryIO, ReplacedQualifiesAConsumeOnly) {
  std::string error;
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 replaced\nend\n",
                            ResolveAll, &error))
      << "alone it describes nothing";
  EXPECT_FALSE(parseSummary("summary\n  effect param 0 written,replaced\nend\n",
                            ResolveAll, &error));
  const auto moved = parseSummary(
      "summary\n  effect param 0 moved(free),replaced\nend\n", ResolveAll);
  ASSERT_TRUE(moved);
  EXPECT_TRUE(moved->effectOf(SummaryPath::param(0)).replaced);
  // A version-3 record without the flag: not replaced.
  const auto plain =
      parseSummary("summary\n  effect param 0 freed(free)\nend\n", ResolveAll);
  ASSERT_TRUE(plain);
  EXPECT_FALSE(plain->effectOf(SummaryPath::param(0)).replaced);
  // So does `element` (RFC 0008, *Element consumes*).
  EXPECT_FALSE(parseSummary("summary\n  effect global g_buf * element\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(
      parseSummary("summary\n  effect global g_buf * read,element\nend\n",
                   ResolveAll, &error));
  const auto element = parseSummary(
      "summary\n  effect global g_buf * freed,element\nend\n", ResolveAll);
  ASSERT_TRUE(element);
  EXPECT_TRUE(element->effectOf(SummaryPath::global(0).deref()).element);
  EXPECT_FALSE(plain->effectOf(SummaryPath::param(0)).element);
}

TEST(SummaryIO, RejectsMalformedPointerValidityLines) {
  std::string error;
  EXPECT_FALSE(parseSummary("summary\n  requires\nend\n", ResolveAll, &error));
  EXPECT_FALSE(
      parseSummary("summary\n  requires x\nend\n", ResolveAll, &error));
  EXPECT_FALSE(
      parseSummary("summary\n  requires 1x\nend\n", ResolveAll, &error));
  EXPECT_FALSE(parseSummary("summary\n  notnull maybe param 0\nend\n",
                            ResolveAll, &error));
  EXPECT_FALSE(
      parseSummary("summary\n  notnull zero\nend\n", ResolveAll, &error));
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
