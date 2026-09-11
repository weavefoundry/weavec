//===- ObjectTypeTest.cpp - Object-view obligations (RFC 0022) -----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/ObjectType.h"

#include "weavec/Core/CheckedIO.h"
#include "weavec/Core/Safety.h"

#include <gtest/gtest.h>

namespace weavec::core {

TEST(ObjectType, IdentityAndElementAlignmentAreIndependentOfEqualSizes) {
  const ObjectType integer{.bytes = 4, .alignment = 4, .identity = "int"};
  const ObjectType floating{.bytes = 4, .alignment = 4, .identity = "float"};
  const ObjectType wide{.bytes = 8, .alignment = 8, .identity = "long"};
  for (std::int64_t offset = -16; offset <= 32; ++offset) {
    EXPECT_EQ(integer.accepts(integer, offset), offset >= 0 && offset % 4 == 0);
    EXPECT_FALSE(integer.accepts(floating, offset));
    EXPECT_FALSE(integer.accepts(wide, offset));
  }
}

TEST(ObjectType, CanonicalRoundTripAndMalformedDescriptors) {
  for (const ObjectType &type :
       {ObjectType{.bytes = 1, .alignment = 1, .identity = "char"},
        ObjectType{.bytes = 24,
                   .alignment = 8,
                   .identity = "record:node{pointer,int}"},
        ObjectType{.bytes = 16, .alignment = 16, .identity = "vector"}}) {
    EXPECT_TRUE(type.valid());
    EXPECT_EQ(ObjectType::parse(type.toString()), type);
  }
  for (const auto *text :
       {"", "0:1:int", "4:0:int", "4:3:int", "4:8:int", "4:4:", "04:4:int",
        "4:04:int", "-4:4:int", "18446744073709551616:4:int", "4:4:int\n"})
    EXPECT_FALSE(ObjectType::parse(text)) << text;
  EXPECT_FALSE(ObjectType::parse("4:4:" + std::string(9000, 'x')));
}

TEST(ObjectType, CheckedTransportPreservesAndValidatesTheDescriptor) {
  CheckedContract contract;
  contract.computed = true;
  contract.signature = "int (void *)";
  contract.require(
      {.kind = CheckedRequirementKind::ObjectType,
       .path = SummaryPath::param(0),
       .other = {},
       .family = ObjectType{.bytes = 4, .alignment = 4, .identity = "int"}
                     .toString()});
  const auto encoded = printCheckedContract(contract, {});
  EXPECT_EQ(parseCheckedContract(encoded, {}), contract);
  contract.requirements.clear();
  contract.require({.kind = CheckedRequirementKind::ObjectType,
                    .path = SummaryPath::param(0),
                    .other = {},
                    .family = "4:3:int"});
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
}

TEST(ObjectType, JoinsCannotRecoverConflictingOrLostViewEvidence) {
  const PlaceId a{1};
  const PlaceId b{2};
  const PlaceId c{3};
  SafetyState first;
  SafetyState second;
  first.objectTypes = {{a, "4:4:int"}, {b, ""}};
  second.objectTypes = {{a, "4:4:float"}, {c, "4:4:int"}};
  first.join(second);
  EXPECT_EQ(first.objectTypes.at(a), "?");
  EXPECT_EQ(first.objectTypes.at(b), "?");
  EXPECT_EQ(first.objectTypes.at(c), "?");
  first.forget(a);
  EXPECT_FALSE(first.objectTypes.contains(a));
  SafetyState same;
  same.objectTypes[b] = "4:4:int";
  auto copy = same;
  copy.join(same);
  EXPECT_EQ(copy.objectTypes, same.objectTypes);
}

TEST(ObjectType, OutputNullConditionIsNotAnEntryAssumption) {
  CheckedContract contract;
  contract.computed = true;
  const CheckedRequirement post{.kind = CheckedRequirementKind::Initialized,
                                .path = SummaryPath::param(0).deref(),
                                .other = {},
                                .end = PathAffine::ofConstant(4),
                                .family = {},
                                .ifNonNull = true};
  contract.establish(post);
  EXPECT_EQ(parseCheckedContract(printCheckedContract(contract, {}), {}),
            contract);
  contract.require(post);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
  contract.requirements.clear();
  contract.establishes.clear();
  auto invalid = post;
  invalid.kind = CheckedRequirementKind::ObjectType;
  invalid.family = "4:4:int";
  contract.establish(invalid);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
}

} // namespace weavec::core
