//===- BufferTest.cpp - Contiguous proof transport (RFC 0026) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Buffer.h"

#include "weavec/Core/CheckedIO.h"
#include "weavec/Core/Safety.h"

#include <gtest/gtest.h>

namespace weavec::core {

static BufferShape bufferShape() {
  return {.object = {.bytes = 24, .alignment = 8, .identity = "record:buffer"},
          .data = {.name = "data", .offset = 0, .bytes = 8},
          .length = {.name = "length", .offset = 8, .bytes = 8},
          .capacity = {.name = "capacity", .offset = 16, .bytes = 8}};
}

static BufferFact bufferFact(bool initialized = true, bool nonNull = false) {
  return {.shape = bufferShape(),
          .object = PlaceId{1},
          .length = PlaceId{3},
          .capacity = PlaceId{4},
          .initialized = initialized,
          .nonNull = nonNull};
}

TEST(BufferShape, CanonicalTargetLayoutsRoundTrip) {
  for (const auto unit : {1U, 4U, 8U}) {
    auto shape = bufferShape();
    shape.elementBytes = unit;
    shape.pointerElements = unit == 8;
    ASSERT_TRUE(shape.valid());
    EXPECT_EQ(BufferShape::decode(shape.encode()), shape);
  }
  auto terminated = bufferShape();
  terminated.terminated = true;
  EXPECT_EQ(BufferShape::decode(terminated.encode()), terminated);
  terminated.pointerElements = true;
  EXPECT_FALSE(terminated.valid());
  terminated.pointerElements = false;
  terminated.elementBytes = 4;
  EXPECT_FALSE(terminated.valid());
}

TEST(BufferShape, ReaderAndWriterCapabilitiesCannotBeInterchanged) {
  const auto writer = bufferShape();
  auto reader = writer;
  reader.reader = true;
  ASSERT_TRUE(reader.valid());
  EXPECT_TRUE(reader.encode().starts_with("reader1:"));
  EXPECT_EQ(BufferShape::decode(reader.encode()), reader);
  EXPECT_FALSE(reader.sameLayoutAs(writer));
  EXPECT_FALSE(reader.entails(writer));
  EXPECT_FALSE(writer.entails(reader));
  reader.ownsBacking = true;
  reader.terminated = true;
  EXPECT_EQ(BufferShape::decode(reader.encode()), reader);
  reader.pointerElements = true;
  EXPECT_FALSE(reader.valid());
  reader.pointerElements = false;
  reader.elementBytes = 4;
  EXPECT_FALSE(reader.valid());
}

TEST(BufferShape, ReaderProofTransportRetainsItsDistinctSemantics) {
  auto shape = bufferShape();
  shape.reader = true;
  CheckedContract contract;
  contract.computed = true;
  contract.require({.kind = CheckedRequirementKind::Buffer,
                    .path = SummaryPath::param(0).deref(),
                    .other = {},
                    .family = shape.encode()});
  contract.establish({.kind = CheckedRequirementKind::Buffer,
                      .path = SummaryPath::param(0).deref(),
                      .other = {},
                      .family = shape.encode()});
  const auto encoded = printCheckedContract(contract, {});
  EXPECT_EQ(parseCheckedContract(encoded, {}), contract);
  auto a = bufferFact();
  auto b = a;
  b.shape.reader = true;
  BufferFacts left;
  BufferFacts right;
  left.set(PlaceId{2}, a);
  right.set(PlaceId{2}, b);
  left.join(right);
  EXPECT_TRUE(left.values.empty());
  EXPECT_TRUE(left.storage.empty());
}

TEST(BufferShape, MalformedDescriptorsCannotPublishEvidence) {
  const auto encoded = bufferShape().encode();
  for (std::size_t n = 0; n < encoded.size(); ++n)
    EXPECT_FALSE(BufferShape::decode(encoded.substr(0, n))) << n;
  EXPECT_FALSE(BufferShape::decode(encoded + "x"));
  EXPECT_FALSE(BufferShape::decode("buffer1:01:" + encoded.substr(10)));
  EXPECT_FALSE(BufferShape::decode(std::string(16385, 'x')));
  auto bad = bufferShape();
  bad.data.offset = UINT64_MAX;
  EXPECT_FALSE(bad.valid());
  bad = bufferShape();
  bad.length.offset = bad.data.offset;
  EXPECT_FALSE(bad.valid());
  bad = bufferShape();
  bad.capacity.name = bad.length.name;
  EXPECT_FALSE(bad.valid());
  bad = bufferShape();
  bad.capacity.bytes = 16;
  EXPECT_FALSE(bad.valid());
  bad = bufferShape();
  bad.elementBytes = 0;
  EXPECT_FALSE(bad.valid());
  bad = bufferShape();
  bad.elementBytes = UINT64_MAX;
  EXPECT_FALSE(bad.valid());
  bad = bufferShape();
  bad.length.name = "length\n";
  EXPECT_FALSE(bad.valid());
}

TEST(BufferFacts, JoinDoesNotInventInitializationOrNonnullStorage) {
  // Enumerate the concrete possibilities independently: a must assertion
  // after a join must hold for every represented predecessor.
  for (unsigned left = 0; left < 4; ++left)
    for (unsigned right = 0; right < 4; ++right) {
      BufferFacts a;
      BufferFacts b;
      a.set(PlaceId{2}, bufferFact((left & 1U) != 0, (left & 2U) != 0));
      b.set(PlaceId{2}, bufferFact((right & 1U) != 0, (right & 2U) != 0));
      auto reverse = b;
      reverse.join(a);
      a.join(b);
      EXPECT_EQ(a, reverse);
      ASSERT_EQ(a.values.size(), 1U);
      const auto &fact = a.values.at(PlaceId{2});
      if (fact.initialized) {
        EXPECT_NE(left & 1U, 0U);
        EXPECT_NE(right & 1U, 0U);
      }
      if (fact.nonNull) {
        EXPECT_NE(left & 2U, 0U);
        EXPECT_NE(right & 2U, 0U);
      }
      EXPECT_FALSE(a.join(a));
      EXPECT_TRUE(a.join({}));
      EXPECT_TRUE(a.values.empty());
    }
}

TEST(BufferFacts, DifferentRecordsAndEveryDependentWriteLoseThePredicate) {
  BufferFacts original;
  original.set(PlaceId{2}, bufferFact());
  for (const auto id : {1U, 2U, 3U, 4U}) {
    auto state = original;
    state.forget(PlaceId{id});
    EXPECT_TRUE(state.values.empty());
  }
  auto unrelated = original;
  unrelated.forget(PlaceId{5});
  EXPECT_EQ(unrelated, original);
  auto different = original;
  different.values.at(PlaceId{2}).shape.object.identity = "other:record";
  original.join(different);
  EXPECT_TRUE(original.values.empty());
}

TEST(BufferFacts, ConditionalBoundsRetireWithEveryCapturedDependency) {
  BufferFacts original;
  original.set(PlaceId{2}, bufferFact());
  PlaceGuard condition;
  condition.require(PlaceId{6}, ValueFact::of(Outcome::Zero));
  original.bounds[PlaceId{2}].push_back({.capacity = PlaceId{4},
                                         .minimum = Affine::ofPlace(PlaceId{5}),
                                         .when = condition});
  for (const auto id : {2U, 4U, 5U, 6U}) {
    auto state = original;
    state.forget(PlaceId{id});
    EXPECT_TRUE(state.bounds.empty());
  }
  auto other = original;
  other.bounds.clear();
  EXPECT_TRUE(original.join(other));
  EXPECT_TRUE(original.bounds.empty());
}

TEST(BufferFacts, ExhaustionIsStickyAndCannotStrengthenAJoin) {
  BufferFacts state;
  for (std::uint32_t i = 0; i < MaxBufferFacts + 1; ++i)
    state.set(PlaceId{i + 10}, bufferFact());
  EXPECT_TRUE(state.limited);
  EXPECT_EQ(state.values.size(), MaxBufferFacts);
  BufferFacts incoming;
  EXPECT_TRUE(incoming.join(state));
  EXPECT_TRUE(incoming.limited);
  EXPECT_TRUE(incoming.values.empty());
}

TEST(BufferContracts, CapacityGuaranteesRoundTripAndAreOutputOnly) {
  auto path = SummaryPath::param(0);
  path.steps.pushBack({.step = PathStep::Deref, .field = {}});
  const CheckedRequirement premise{.kind = CheckedRequirementKind::Buffer,
                                   .path = path,
                                   .other = {},
                                   .family = bufferShape().encode()};
  CheckedContract contract;
  contract.computed = true;
  contract.require(premise);
  auto output = premise;
  output.end = PathAffine::ofPath(SummaryPath::param(1));
  output.on = Outcome::Zero;
  contract.establish(output);
  EXPECT_EQ(parseCheckedContract(printCheckedContract(contract, {}), {}),
            contract);
  auto bad = contract;
  output.on.reset();
  bad.require(output);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
  bad = contract;
  output = premise;
  output.end = PathAffine::ofConstant(-1);
  bad.establish(output);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
  bad = contract;
  output = premise;
  output.family += 'x';
  bad.establish(output);
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
}

TEST(BufferFacts, BackingRecordsDoNotRetainLengthOrElementCapabilities) {
  auto fact = bufferFact();
  fact.shape.terminated = true;
  BufferFacts state;
  state.set(PlaceId{2}, fact);
  EXPECT_FALSE(state.storage.at(PlaceId{2}).initialized);
  EXPECT_FALSE(state.storage.at(PlaceId{2}).shape.terminated);
  fact.shape.elementBytes = 0;
  state.set(PlaceId{2}, fact);
  EXPECT_TRUE(state.storage.empty());
  EXPECT_TRUE(state.values.empty());
  EXPECT_TRUE(state.limited);
}

TEST(BufferFacts, SeparationIdentityRequiresEveryPredecessor) {
  auto fact = bufferFact();
  fact.entryBacking = PlaceId{2};
  BufferFacts a;
  BufferFacts b;
  a.set(PlaceId{2}, fact);
  b.set(PlaceId{2}, fact);
  EXPECT_FALSE(a.join(b));
  fact.entryBacking = PlaceId{9};
  b.set(PlaceId{2}, fact);
  EXPECT_TRUE(a.join(b));
  EXPECT_FALSE(a.values.at(PlaceId{2}).entryBacking);
  EXPECT_FALSE(a.storage.at(PlaceId{2}).entryBacking);
}

TEST(BufferFacts, DeferredGuaranteesDependOnTheCallAndEveryContainerField) {
  BufferFacts original;
  BufferPost post{.fact = bufferFact(), .when = {}};
  post.when.require(PlaceId{8}, ValueFact::of(Outcome::Zero));
  original.pending[PlaceId{2}].push_back(post);
  for (const auto dependency : {1U, 2U, 3U, 4U, 8U}) {
    auto state = original;
    state.forget(PlaceId{dependency});
    EXPECT_TRUE(state.pending.empty());
  }
  auto unchanged = original;
  unchanged.forget(PlaceId{9});
  EXPECT_EQ(unchanged, original);
  EXPECT_FALSE(unchanged.join(original));
  EXPECT_TRUE(unchanged.join({}));
  EXPECT_TRUE(unchanged.pending.empty());
}

TEST(BufferContracts, SequenceOutputsRequirePointerCellsAndCannotBeInputs) {
  auto path = SummaryPath::param(0);
  path.steps.pushBack({.step = PathStep::Deref, .field = {}});
  auto shape = bufferShape();
  shape.pointerElements = true;
  shape.elementBytes = 8;
  for (const auto kind : {CheckedRequirementKind::BufferPreserved,
                          CheckedRequirementKind::BufferAppended}) {
    CheckedContract contract;
    contract.computed = true;
    CheckedRequirement post{.kind = kind,
                            .path = path,
                            .other =
                                kind == CheckedRequirementKind::BufferPreserved
                                    ? path
                                    : SummaryPath::param(1),
                            .family = shape.encode(),
                            .on = Outcome::Zero};
    contract.establish(post);
    EXPECT_EQ(parseCheckedContract(printCheckedContract(contract, {}), {}),
              contract);
    auto invalid = contract;
    invalid.require(post);
    EXPECT_FALSE(parseCheckedContract(printCheckedContract(invalid, {}), {}));
    invalid = {};
    invalid.computed = true;
    post.family = bufferShape().encode();
    invalid.establish(post);
    EXPECT_FALSE(parseCheckedContract(printCheckedContract(invalid, {}), {}));
  }
}

} // namespace weavec::core
