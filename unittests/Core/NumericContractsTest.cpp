//===- NumericContractsTest.cpp - RFC 0017 numeric summary contracts ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/SummaryIO.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace weavec::core {

using NumericExpression = IntegerExpression<SummaryPath>;
static constexpr IntegerType ContractType{.width = 32, .isSigned = true};

static NumericExpression number(std::int64_t value) {
  return NumericExpression::constant(
      IntegerValue::ofBits(ContractType, static_cast<std::uint64_t>(value)));
}

static PathGuard equalTo(SummaryPath path, std::int64_t value) {
  PathGuard guard;
  guard.requireInteger(
      {.lhs = NumericExpression::input(std::move(path), ContractType),
       .op = IntegerOp::Equal,
       .rhs = number(value)});
  return guard;
}

static std::string globalName(std::uint32_t id) {
  return "g" + std::to_string(id);
}

static std::optional<std::uint32_t> resolveGlobal(std::string_view name) {
  for (std::uint32_t id = 0; id < 8; ++id)
    if (name == globalName(id))
      return id;
  return std::nullopt;
}

TEST(NumericContracts, RequirementAlternativesRetainTheirGuardDisjunction) {
  const auto need = PathAffine::ofConstant(16);
  const auto start = PathAffine::ofConstant(0);
  const ExtentRequirement zero{
      .need = need, .when = equalTo(SummaryPath::param(1), 0), .start = start};
  const ExtentRequirement two{
      .need = need, .when = equalTo(SummaryPath::param(1), 2), .start = start};
  FunctionSummary lhs;
  FunctionSummary rhs;
  lhs.addRequirement(0, zero);
  rhs.addRequirement(0, two);
  auto reverse = rhs;
  reverse.join(lhs);
  lhs.join(rhs);
  EXPECT_EQ(lhs, reverse);
  EXPECT_EQ(lhs.requiresExtent.at(0), (std::set<ExtentRequirement>{zero, two}));
  lhs.join(lhs);
  EXPECT_EQ(lhs, reverse);
  // Neither access occurs for argument 1. Joining the two guards by their
  // common conjuncts would create an unconditional caller error here.
  for (const auto &requirement : lhs.requiresExtent.at(0)) {
    ASSERT_EQ(requirement.when.integers.size(), 1U);
    EXPECT_EQ(requirement.when.integers.front().evaluate([](const SummaryPath &,
                                                            IntegerType type) {
      return IntegerRange::singleton(IntegerValue::ofBits(type, 1));
    }),
              false);
  }
  EXPECT_EQ(parseSummary(printSummary(lhs, globalName), resolveGlobal), lhs);
}

TEST(NumericContracts, FirstByteIsPartOfRequirementIdentityAndSubsumption) {
  FunctionSummary summary;
  const auto guard = equalTo(SummaryPath::param(1), 1);
  for (const auto &start :
       {std::optional<PathAffine>{}, std::optional(PathAffine::ofConstant(0)),
        std::optional(PathAffine::ofConstant(-4))})
    summary.addRequirement(
        0, {.need = PathAffine::ofConstant(0), .when = guard, .start = start});
  ASSERT_EQ(summary.requiresExtent.at(0).size(), 3U);
  // An empty range, a zero-byte endpoint and a negative access cannot be
  // merged just because their required exclusive endpoint is the same.
  summary.addRequirement(0, {.need = PathAffine::ofConstant(0),
                             .start = PathAffine::ofConstant(-4)});
  EXPECT_EQ(summary.requiresExtent.at(0).size(), 3U);
  EXPECT_EQ(
      std::ranges::count_if(summary.requiresExtent.at(0),
                            [](const auto &r) { return r.when.trivial(); }),
      1);
  const auto text = printSummary(summary, globalName);
  EXPECT_NE(text.find("start -4"), std::string::npos);
  EXPECT_EQ(parseSummary(text, resolveGlobal), summary);
}

TEST(NumericContracts, SixteenRequirementAlternativesDoNotWeakenAtTheCap) {
  FunctionSummary summary;
  for (unsigned i = 0; i < 16; ++i)
    summary.addRequirement(0, {.need = PathAffine::ofConstant(8),
                               .when = equalTo(SummaryPath::param(1), i),
                               .start = PathAffine::ofConstant(0)});
  const auto before = summary.requiresExtent.at(0);
  ASSERT_EQ(before.size(), 16U);
  EXPECT_TRUE(summary.incomplete.empty());
  summary.addRequirement(0, *before.begin());
  EXPECT_TRUE(summary.incomplete.empty());
  summary.addRequirement(0, {.need = PathAffine::ofConstant(8),
                             .when = equalTo(SummaryPath::param(1), 16),
                             .start = PathAffine::ofConstant(0)});
  EXPECT_EQ(summary.requiresExtent.at(0), before);
  EXPECT_TRUE(
      summary.incomplete.contains("extent requirement alternatives exceeded"));
  summary.addRequirement(1, {.need = PathAffine::ofConstant(4)});
  EXPECT_EQ(summary.requiresExtent.at(1).size(), 1U);
  // A truly unconditional requirement may subsume matching alternatives.
  summary.addRequirement(0, {.need = PathAffine::ofConstant(8),
                             .start = PathAffine::ofConstant(0)});
  ASSERT_EQ(summary.requiresExtent.at(0).size(), 1U);
  EXPECT_TRUE(summary.requiresExtent.at(0).begin()->when.trivial());
}

TEST(NumericContracts, NumericGuardBudgetIncludesScalarAndPointerPremises) {
  PathGuard guard;
  guard.require(SummaryPath::param(0), ValueFact::of(Outcome::Positive));
  guard.requirePointer(SummaryPath::param(1), SummaryPath::param(2), true);
  for (unsigned i = 0; i < 6; ++i)
    EXPECT_TRUE(guard.requireInteger(
        equalTo(SummaryPath::param(3 + i), i).integers.front()));
  ASSERT_EQ(guard.size(), 8U);
  const auto before = guard;
  EXPECT_FALSE(
      guard.requireInteger(equalTo(SummaryPath::param(9), 9).integers.front()));
  EXPECT_FALSE(guard.requireInteger(guard.integers.front()));
  EXPECT_EQ(guard, before);
  FunctionSummary summary;
  summary.addRequirement(0, {.need = PathAffine::ofConstant(8), .when = guard});
  EXPECT_EQ(parseSummary(printSummary(summary, globalName), resolveGlobal),
            summary);
  const auto ninth = "summary\n  requires-extent 0 8" +
                     printGuard(guard, globalName) +
                     " and param 10 positive\nend\n";
  EXPECT_FALSE(parseSummary(ninth, resolveGlobal));
}

TEST(NumericContracts, MissingNumericOutputInAReturningBranchIsUnknown) {
  const auto result = SummaryPath::result();
  FunctionSummary exact;
  exact.addNumericOutput(result, {.value = number(7)});
  FunctionSummary bottom;
  bottom.join(exact);
  EXPECT_EQ(bottom, exact);
  exact.join(FunctionSummary{});
  EXPECT_EQ(exact, bottom);
  FunctionSummary returning;
  returning.addOutcome(Outcome::Positive);
  auto reverse = returning;
  reverse.join(exact);
  exact.join(returning);
  EXPECT_EQ(exact, reverse);
  EXPECT_EQ(exact.numericOutputs.at(result),
            (std::set<NumericOutput>{NumericOutput{}}));
}

TEST(NumericContracts, MergedUnknownNumericOutputsAbsorbValuesInEveryOrder) {
  const auto result = SummaryPath::result();
  const std::array<NumericOutput, 3> outputs{
      NumericOutput{.value = number(7)},
      NumericOutput{.when = equalTo(SummaryPath::param(0), 0)},
      NumericOutput{.when = equalTo(SummaryPath::param(0), 1)}};
  FunctionSummary expected;
  expected.addNumericOutput(result, NumericOutput{});
  std::array<unsigned, 3> order{0U, 1U, 2U};
  do {
    SCOPED_TRACE(::testing::PrintToString(order));
    FunctionSummary summary;
    for (const auto index : order)
      summary.addNumericOutput(result, outputs.at(index));
    EXPECT_EQ(summary, expected);
    summary.addNumericOutput(result, {.value = number(9)});
    EXPECT_EQ(summary, expected);
  } while (std::ranges::next_permutation(order).found);
}

TEST(NumericContracts, NumericOutputJoinIsCanonicalAcrossOrdersAndGroupings) {
  const auto result = SummaryPath::result();
  std::array<FunctionSummary, 3> summaries;
  summaries[0].addNumericOutput(result, {.value = number(7)});
  summaries[1].addNumericOutput(result,
                                {.when = equalTo(SummaryPath::param(0), 0)});
  summaries[2].addNumericOutput(result,
                                {.when = equalTo(SummaryPath::param(0), 1)});
  FunctionSummary expected;
  expected.addNumericOutput(result, NumericOutput{});
  std::array<unsigned, 3> order{0U, 1U, 2U};
  do {
    SCOPED_TRACE(::testing::PrintToString(order));
    auto left = summaries.at(order[0]);
    left.join(summaries.at(order[1]));
    left.join(summaries.at(order[2]));
    EXPECT_EQ(left, expected);

    auto tail = summaries.at(order[1]);
    tail.join(summaries.at(order[2]));
    auto right = summaries.at(order[0]);
    right.join(tail);
    EXPECT_EQ(right, expected);
    left.join(right);
    EXPECT_EQ(left, expected);
  } while (std::ranges::next_permutation(order).found);

  auto self = summaries[0];
  self.join(self);
  EXPECT_EQ(self, summaries[0]);
}

TEST(NumericContracts, MergedConditionalUnknownPreservesOtherNumericOutputs) {
  const auto result = SummaryPath::result();
  PathGuard shared;
  shared.require(SummaryPath::param(1), ValueFact::of(Outcome::Positive));
  auto zero = equalTo(SummaryPath::param(0), 0);
  zero.conjoin(shared);
  auto one = equalTo(SummaryPath::param(0), 1);
  one.conjoin(shared);
  FunctionSummary summary;
  summary.addNumericOutput(result, {.value = number(7)});
  summary.addNumericOutput(result, {.when = zero});
  summary.addNumericOutput(result, {.when = one});
  EXPECT_EQ(summary.numericOutputs.at(result),
            (std::set<NumericOutput>{NumericOutput{.value = number(7)},
                                     NumericOutput{.when = shared}}));
}

TEST(NumericContracts, NumericOutputGuardsAndDependenciesRemapTogether) {
  const auto destination = SummaryPath::global(0).field("count");
  const auto left =
      NumericExpression::input(SummaryPath::global(1).field("n"), ContractType);
  const auto right = NumericExpression::input(
      SummaryPath::global(2).deref().field("limit"), ContractType);
  const auto product =
      NumericExpression::operation(IntegerOp::Multiply, left, right);
  ASSERT_TRUE(product);
  PathGuard guard;
  guard.requireInteger({.lhs = left, .op = IntegerOp::Less, .rhs = right});
  guard.require(SummaryPath::global(3), ValueFact::of(Outcome::Positive));
  FunctionSummary summary;
  summary.addNumericOutput(destination, {.value = product, .when = guard});
  summary.addNumericOutput(SummaryPath::param(0).deref(), {.value = number(3)});
  summary.addRequirement(0, {.need = PathAffine::ofExpression(*product, 4, 4),
                             .when = guard,
                             .start = PathAffine::ofExpression(left, 4, 0)});
  const auto mapped = remapGlobals(
      summary, [](std::uint32_t id) { return std::optional(id + 100); });
  const auto &output =
      *mapped.numericOutputs.at(SummaryPath::global(100).field("count"))
           .begin();
  ASSERT_TRUE(output.value);
  EXPECT_TRUE(output.value->dependsOn(SummaryPath::global(101).field("n")));
  EXPECT_TRUE(
      output.value->dependsOn(SummaryPath::global(102).deref().field("limit")));
  EXPECT_FALSE(output.value->dependsOn(SummaryPath::global(1).field("n")));
  ASSERT_EQ(output.when.integers.size(), 1U);
  EXPECT_TRUE(output.when.integers.front().dependsOn(
      SummaryPath::global(102).deref().field("limit")));
  EXPECT_TRUE(output.when.conditions.contains(SummaryPath::global(103)));
  const auto &requirement = *mapped.requiresExtent.at(0).begin();
  ASSERT_TRUE(requirement.start);
  ASSERT_TRUE(requirement.start->expression);
  EXPECT_TRUE(requirement.start->expression->dependsOn(
      SummaryPath::global(101).field("n")));
  EXPECT_EQ(requirement.need.scale, 4);
  EXPECT_EQ(requirement.need.constant, 4);
  EXPECT_EQ(requirement.when, output.when);
  EXPECT_TRUE(mapped.incomplete.empty());
  for (const auto missing : {1U, 2U, 3U}) {
    const auto dropped = remapGlobals(summary, [missing](std::uint32_t id) {
      return id == missing ? std::nullopt : std::optional(id);
    });
    EXPECT_EQ(dropped.numericOutputs.at(destination),
              (std::set<NumericOutput>{NumericOutput{}}));
    EXPECT_TRUE(dropped.requiresExtent.empty());
    EXPECT_FALSE(dropped.incomplete.empty());
    EXPECT_EQ(dropped.numericOutputs.at(SummaryPath::param(0).deref()),
              summary.numericOutputs.at(SummaryPath::param(0).deref()));
  }
}

TEST(NumericContracts, MissingFirstByteDependencyInvalidatesWholeRequirement) {
  FunctionSummary summary;
  const auto start =
      NumericExpression::input(SummaryPath::global(1), ContractType);
  summary.addRequirement(0, {.need = PathAffine::ofConstant(8),
                             .start = PathAffine::ofExpression(start)});
  const auto dropped =
      remapGlobals(summary, [](std::uint32_t) -> std::optional<std::uint32_t> {
        return std::nullopt;
      });
  EXPECT_TRUE(dropped.requiresExtent.empty());
  EXPECT_TRUE(dropped.incomplete.contains(
      "extent requirement dependency global is unavailable"));
}

static std::optional<std::uint32_t> declineLastGlobal(std::string_view name) {
  return name == "g7" ? std::nullopt : resolveGlobal(name);
}

TEST(NumericContracts, MissingPremiseInvalidatesWholeParsedContract) {
  const auto input =
      NumericExpression::input(SummaryPath::global(7), ContractType);
  const auto expression = input.toString([](const SummaryPath &path) {
    return printSummaryPath(path, globalName);
  });
  const auto range =
      IntegerRange::between(IntegerValue::ofBits(ContractType, 0),
                            IntegerValue::ofBits(ContractType, 8));
  const std::vector<std::string> guards{
      "global g7 positive and param 1 positive",
      "param 2 same global g7 and param 1 positive",
      "cmp " + expression + " lt i32,c,4 and param 1 positive",
      "cmp i32,c,0 lt " + expression + " and param 1 positive",
      "cmp " + expression + " in " + range.toString() +
          " and param 1 positive"};
  for (const auto &guard : guards)
    for (const auto *const contract :
         {"requires-extent 0 8 start 0", "numeric result value i32,c,7"}) {
      const auto text = std::string("summary\n  ") + contract + " when " +
                        guard + "\n  numeric param 3 * value i32,c,9\nend\n";
      const auto parsed = parseSummary(text, declineLastGlobal);
      ASSERT_TRUE(parsed) << text;
      EXPECT_TRUE(parsed->requiresExtent.empty());
      if (const auto found = parsed->numericOutputs.find(SummaryPath::result());
          found != parsed->numericOutputs.end())
        EXPECT_EQ(found->second, (std::set<NumericOutput>{NumericOutput{}}));
      EXPECT_FALSE(parsed->incomplete.empty());
      EXPECT_EQ(parsed->numericOutputs.at(SummaryPath::param(3).deref()),
                (std::set<NumericOutput>{NumericOutput{.value = number(9)}}));
    }
}

TEST(NumericContracts,
     MissingExtentAndFirstByteDependenciesDropOnlyTheirRequirement) {
  const auto input =
      NumericExpression::input(SummaryPath::global(7), ContractType);
  const auto expression =
      printAffine(PathAffine::ofExpression(input), globalName);
  for (const auto &bounds : std::vector<std::string>{
           "global g7 scale 1 plus 0 start 0",
           "8 start global g7 scale 1 plus 0", expression + " start 0",
           "8 start " + expression}) {
    const auto parsed = parseSummary("summary\n  requires-extent 0 " + bounds +
                                         "\n  requires-extent 1 4\nend\n",
                                     declineLastGlobal);
    ASSERT_TRUE(parsed) << bounds;
    EXPECT_FALSE(parsed->requiresExtent.contains(0));
    ASSERT_TRUE(parsed->requiresExtent.contains(1));
    EXPECT_EQ(parsed->requiresExtent.at(1).begin()->need,
              PathAffine::ofConstant(4));
    EXPECT_TRUE(parsed->incomplete.contains(
        "extent requirement dependency global is unavailable"));
  }
}

TEST(NumericContracts,
     MissingOutputValueIsUnknownAndMissingDestinationIsIncomplete) {
  FunctionSummary summary;
  const auto input =
      NumericExpression::input(SummaryPath::global(7), ContractType);
  const auto guard = equalTo(SummaryPath::param(1), 3);
  summary.addNumericOutput(SummaryPath::result(),
                           {.value = input, .when = guard});
  summary.addNumericOutput(SummaryPath::global(7), {.value = number(4)});
  const auto parsed =
      parseSummary(printSummary(summary, globalName), declineLastGlobal);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->numericOutputs.at(SummaryPath::result()),
            (std::set<NumericOutput>{NumericOutput{.when = guard}}));
  EXPECT_FALSE(parsed->numericOutputs.contains(SummaryPath::global(7)));
  EXPECT_FALSE(parsed->numericOutputs.contains(SummaryPath::global(0)));
  EXPECT_TRUE(
      parsed->incomplete.contains("numeric output global is unavailable"));
  EXPECT_TRUE(parsed->incomplete.contains(
      "numeric output dependency global is unavailable"));
  EXPECT_EQ(*parsed, remapGlobals(summary, [](std::uint32_t id) {
    return id == 7 ? std::nullopt : std::optional(id);
  }));
}

TEST(NumericContracts,
     MissingArrayGuardWeakensDefiniteWritesInParsingAndRemapping) {
  for (const bool numeric : {false, true}) {
    PathGuard guard;
    guard.require(SummaryPath::param(1), ValueFact::of(Outcome::Positive));
    if (numeric)
      guard.requireInteger(equalTo(SummaryPath::global(7), 1).integers.front());
    else
      guard.require(SummaryPath::global(7), ValueFact::of(Outcome::Positive));
    FunctionSummary summary;
    const auto storage = SummaryPath::param(0).deref();
    summary.arrayFills.insert({.storage = storage,
                               .count = PathAffine::ofConstant(3),
                               .bytes = std::nullopt,
                               .when = guard,
                               .definite = true});
    summary.arrayReleases.insert({.storage = storage,
                                  .begin = PathAffine::ofConstant(0),
                                  .count = PathAffine::ofConstant(3),
                                  .when = guard,
                                  .cleared = true,
                                  .definite = true});
    summary.arrayCopies.insert({.dest = storage,
                                .source = SummaryPath::param(2).deref(),
                                .destBegin = PathAffine::ofConstant(0),
                                .sourceBegin = PathAffine::ofConstant(0),
                                .count = PathAffine::ofConstant(3),
                                .elementBytes = 8,
                                .view = {},
                                .when = guard,
                                .definite = true});
    const auto parsed =
        parseSummary(printSummary(summary, globalName), declineLastGlobal);
    ASSERT_TRUE(parsed);
    const auto mapped = remapGlobals(summary, [](std::uint32_t id) {
      return id == 7 ? std::nullopt : std::optional(id);
    });
    for (const auto &weakened : {*parsed, mapped}) {
      ASSERT_EQ(weakened.arrayFills.size(), 1U);
      ASSERT_EQ(weakened.arrayReleases.size(), 1U);
      ASSERT_EQ(weakened.arrayCopies.size(), 1U);
      EXPECT_FALSE(weakened.arrayFills.begin()->definite);
      EXPECT_FALSE(weakened.arrayReleases.begin()->definite);
      EXPECT_FALSE(weakened.arrayCopies.begin()->definite);
      EXPECT_EQ(weakened.arrayFills.begin()->when.size(), 1U);
      EXPECT_TRUE(weakened.incomplete.contains(
          "array range guard lost in program interface"));
    }
  }
}

TEST(NumericContracts, MissingGlobalDoesNotBypassExpressionValidation) {
  const auto input =
      NumericExpression::input(SummaryPath::global(7), ContractType);
  const auto expression = input.toString([](const SummaryPath &path) {
    return printSummaryPath(path, globalName);
  });
  EXPECT_FALSE(parseSummary("summary\n  numeric result value " + expression +
                                ";i32,bogus\nend\n",
                            declineLastGlobal));
  EXPECT_FALSE(parseSummary("summary\n  requires-extent 0 8 when cmp " +
                                expression + " eq u8,c,0\nend\n",
                            declineLastGlobal));
}

TEST(NumericContracts,
     NumericExpressionPathsAreParsedAsCompleteInterfacePaths) {
  const auto parse = [](std::string_view text) {
    return NumericExpression::parse(text, [](std::string_view path) {
      return parseSummaryPath(path, resolveGlobal);
    });
  };
  // Build the byte encoding without relying on the path printer to validate it.
  const auto encoded = [](std::string_view path) {
    constexpr std::string_view Hex = "0123456789abcdef";
    std::string text = "i32,v,";
    for (const char character : path) {
      const auto byte = static_cast<unsigned char>(character);
      text += Hex[byte >> 4U];
      text += Hex[byte & 15U];
    }
    return text;
  };
  for (const auto *const path :
       {"param 0 *.count", "global g1 .n", "result .count", "param 2 []*.n"})
    EXPECT_TRUE(parse(encoded(path))) << path;
  for (const auto *const path :
       {"local 0", "param", "param -1", "param 4294967296", "param 0 .",
        "param 0 [", "param 0 [bad]", "result 0", "param 0 trailing"}) {
    EXPECT_FALSE(parse(encoded(path))) << path;
    EXPECT_FALSE(parseSummary("summary\n  numeric result value " +
                                  encoded(path) + "\nend\n",
                              resolveGlobal))
        << path;
  }
  EXPECT_FALSE(parse(encoded("global missing")));
  EXPECT_FALSE(
      parse(encoded("param 0 ." + std::string(MaxIntegerExpressionText, 'x'))));
}

} // namespace weavec::core
