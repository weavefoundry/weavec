//===- FormatTest.cpp - Runtime format contracts (RFC 0024) -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "weavec/Core/Format.h"

#include "weavec/Core/CheckedIO.h"

#include <gtest/gtest.h>

namespace weavec::core {
TEST(OutputFormatTest, SequentialArgumentsIncludeWidthAndPrecision) {
  const auto format = OutputFormat::parse("a%%b %*.*s:%lld:%zu:%Lf");
  ASSERT_TRUE(format.valid()) << format.error;
  EXPECT_EQ(format.literalBytes, 7U);
  EXPECT_EQ(format.arguments, 6U);
  ASSERT_EQ(format.conversions.size(), 4U);
  EXPECT_EQ(format.conversions[0].widthArgument, 0U);
  EXPECT_EQ(format.conversions[0].precisionArgument, 1U);
  EXPECT_EQ(format.conversions[0].argument, 2U);
  EXPECT_EQ(format.conversions[1].type, FormatType::LongLong);
  EXPECT_EQ(format.conversions[2].type, FormatType::Size);
  EXPECT_EQ(format.conversions[3].type, FormatType::LongDouble);
}
TEST(OutputFormatTest, UnsupportedSyntaxNeverProducesACompleteFormat) {
  for (const auto *text : {"%", "%n", "%2$d", "%*2$d", "%ls", "%lc", "%Ld",
                           "%hs", "%.2c", "%+s", "%#c", "%q", "%2147483648d"})
    EXPECT_FALSE(OutputFormat::parse(text).valid()) << text;
  EXPECT_FALSE(
      OutputFormat::parse(std::string(MaxFormatBytes + 1, 'x')).valid());
  std::string tooMany;
  for (std::size_t i = 0; i <= MaxFormatConversions; ++i)
    tooMany += "%d";
  EXPECT_FALSE(OutputFormat::parse(tooMany).valid());
}
TEST(OutputFormatTest, TypesAndLiteralBoundaries) {
  for (const auto *text :
       {"%d %i %u %o %x %X", "%hhd %hd %ld %lld %jd %zd %td",
        "%hhu %hu %lu %llu %ju %zu %tu", "%f %lf %Lf %a %g %e", "%s %c %p %%",
        "%#08x %-10.3s"})
    EXPECT_TRUE(OutputFormat::parse(text).valid()) << text;
  const auto format = OutputFormat::parse(std::string_view("ok\0%n", 5));
  EXPECT_TRUE(format.valid());
  EXPECT_EQ(format.literalBytes, 2U);
  EXPECT_TRUE(OutputFormat::parse("").valid());
}
TEST(OutputFormatTest, PortableLiteralRoundTripAndMalformedBytes) {
  const std::string text("%s\0\xff", 4);
  EXPECT_EQ(decodeFormatLiteral(encodeFormatLiteral(text)), text);
  for (const auto *bad : {"", "literal:f", "literal:zz", "other:00"})
    EXPECT_FALSE(decodeFormatLiteral(bad));
  EXPECT_FALSE(decodeFormatLiteral("literal:" +
                                   std::string((MaxFormatBytes * 2) + 2, '0')));
}
TEST(RuntimeSafetyTest, BoundedTerminationDoesNotInitializeCapacity) {
  SafetyState state;
  const PlaceId storage{1};
  state.initialize(storage, {.begin = Affine::ofConstant(0),
                             .end = Affine::ofConstant(16),
                             .terminatedWithin = true});
  EXPECT_TRUE(state.memory.empty());
  EXPECT_EQ(state.boundedTermination.at(storage).size(), 1U);
  auto joined = state;
  EXPECT_TRUE(joined.join(SafetyState{}));
  EXPECT_TRUE(joined.boundedTermination.empty());
  state.forgetZeros();
  EXPECT_TRUE(state.boundedTermination.empty());
}
TEST(RuntimeSafetyTest,
     ConditionalInitializationDoesNotActivateAnArgumentList) {
  SafetyState state;
  const PlaceId list{1};
  state.argumentLists[list] = {
      .phase = ArgumentListPhase::Active, .first = 1, .needsEnd = true};
  state.join(SafetyState{});
  EXPECT_EQ(state.argumentLists.at(list).phase, ArgumentListPhase::Unknown);
  EXPECT_TRUE(state.argumentLists.at(list).needsEnd);
}
TEST(RuntimeSafetyTest, RuntimeRecordsRoundTripAndValidateRoles) {
  CheckedContract contract;
  contract.computed = true;
  contract.require({.kind = CheckedRequirementKind::FormatArguments,
                    .path = SummaryPath::param(0),
                    .other = {},
                    .begin = PathAffine::ofConstant(1),
                    .family = encodeFormatLiteral("%s")});
  const auto encoded = printCheckedContract(contract, {});
  ASSERT_FALSE(encoded.empty());
  EXPECT_EQ(parseCheckedContract(encoded, {}), contract);
  auto invalid = contract;
  auto requirement = *invalid.requirements.begin();
  invalid.requirements.clear();
  requirement.begin = PathAffine::ofConstant(-2);
  invalid.require(requirement);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(invalid, {}), {}));
  invalid = contract;
  invalid.establish(requirement);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(invalid, {}), {}));
}
TEST(RuntimeSafetyTest, StandardStreamRequirementsAreInputOnlyAndCanonical) {
  CheckedContract contract;
  contract.computed = true;
  const CheckedRequirement stream{.kind =
                                      CheckedRequirementKind::StandardStream,
                                  .path = {},
                                  .other = {},
                                  .family = "stdout"};
  contract.require(stream);
  EXPECT_EQ(parseCheckedContract(printCheckedContract(contract, {}), {}),
            contract);
  for (unsigned change = 0; change < 5; ++change) {
    auto invalid = stream;
    if (change == 0)
      invalid.path = SummaryPath::param(1);
    else if (change == 1)
      invalid.other = SummaryPath::param(1);
    else if (change == 2)
      invalid.begin = PathAffine::ofConstant(1);
    else if (change == 3)
      invalid.end = PathAffine::ofConstant(1);
    else
      invalid.family = "arbitrary";
    auto bad = contract;
    bad.requirements.clear();
    bad.require(invalid);
    EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
  }
  contract.establish(stream);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
}
TEST(RuntimeSafetyTest,
     NonnegativeReturnClassesMergeWithoutInitializingFailure) {
  SafetyState state;
  const PlaceId storage{1};
  const PlaceId result{2};
  for (const auto outcome : {Outcome::Zero, Outcome::Positive}) {
    PlaceGuard when;
    when.require(result, ValueFact::of(outcome));
    state.initialize(storage, {.begin = Affine::ofConstant(0),
                               .end = Affine::ofConstant(4),
                               .when = when});
    state.initialize(storage, {.begin = Affine::ofConstant(0),
                               .end = Affine::ofConstant(8),
                               .when = when,
                               .terminatedWithin = true});
  }
  ASSERT_EQ(state.memory.at(storage).size(), 1U);
  ASSERT_EQ(state.boundedTermination.at(storage).size(), 1U);
  const auto &when =
      state.memory.at(storage).front().when.conditions.at(result);
  EXPECT_TRUE(ValueFact::of(Outcome::Zero).implies(when));
  EXPECT_TRUE(ValueFact::of(Outcome::Positive).implies(when));
  EXPECT_FALSE(ValueFact::of(Outcome::Negative).implies(when));
  state.forgetDependency(result);
  EXPECT_TRUE(state.memory.at(storage).empty());
  EXPECT_TRUE(state.boundedTermination.empty());
}
TEST(RuntimeSafetyTest, AJoinRetiresEveryPossiblyConsumedCallerCursor) {
  CheckedContract left;
  CheckedContract right;
  left.computed = right.computed = true;
  const CheckedRequirement consumed{
      .kind = CheckedRequirementKind::ArgumentListConsumed,
      .path = SummaryPath::param(0),
      .other = {},
      .family = {}};
  right.establish(consumed);
  left.join(right);
  EXPECT_TRUE(left.establishes.contains(consumed));
  right = CheckedContract{};
  right.computed = true;
  left.join(right);
  EXPECT_TRUE(left.establishes.contains(consumed));
}
TEST(RuntimeSafetyTest, MalformedListAndTerminationRecordsAreRejected) {
  const auto rejects = [](CheckedRequirement requirement, bool output) {
    CheckedContract contract;
    contract.computed = true;
    if (output)
      contract.establish(std::move(requirement));
    else
      contract.require(std::move(requirement));
    return !parseCheckedContract(printCheckedContract(contract, {}), {});
  };
  EXPECT_TRUE(rejects({.kind = CheckedRequirementKind::ArgumentListConsumed,
                       .path = SummaryPath::param(0),
                       .other = {},
                       .family = {}},
                      false));
  EXPECT_TRUE(rejects({.kind = CheckedRequirementKind::ArgumentList,
                       .path = SummaryPath::param(0),
                       .other = {},
                       .family = {}},
                      true));
  EXPECT_TRUE(rejects({.kind = CheckedRequirementKind::FormatArguments,
                       .path = SummaryPath::param(0),
                       .other = {},
                       .begin = PathAffine::ofConstant(MaxFormatArguments + 1),
                       .family = {}},
                      false));
  EXPECT_TRUE(rejects({.kind = CheckedRequirementKind::TerminatedWithin,
                       .path = SummaryPath::param(0),
                       .other = {},
                       .begin = PathAffine::ofConstant(4),
                       .end = PathAffine::ofConstant(4),
                       .family = {}},
                      true));
}
} // namespace weavec::core
