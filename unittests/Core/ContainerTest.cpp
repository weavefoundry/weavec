//===- ContainerTest.cpp - Linked storage proof rules (RFC 0023) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Container.h"

#include "weavec/Core/CheckedIO.h"
#include "weavec/Core/Safety.h"

#include <gtest/gtest.h>

namespace weavec::core {

static ContainerShape shape(ContainerAccess access = ContainerAccess::Read) {
  return {.object = {.bytes = 16, .alignment = 8, .identity = "record:node"},
          .link = {.name = "next", .offset = 8, .bytes = 8},
          .initialized = {{.name = "next", .offset = 8, .bytes = 8},
                          {.name = "value", .offset = 0, .bytes = 4}},
          .payloads = {},
          .family = access == ContainerAccess::Release ? "free" : "",
          .access = access};
}

static ContainerNode node(ContainerEdge next = ContainerEdge::null()) {
  const auto descriptor = shape();
  return {.object = descriptor.object,
          .initialized = {descriptor.initialized.begin(),
                          descriptor.initialized.end()},
          .next = next,
          .payloads = {},
          .family = "free",
          .live = true,
          .writable = true,
          .allocationBase = true};
}

TEST(ContainerShape, RoundTripIsCanonicalAndCapabilitiesDoNotInventOwnership) {
  for (const auto access : {ContainerAccess::Read, ContainerAccess::Write,
                            ContainerAccess::Release}) {
    const auto value = shape(access);
    ASSERT_TRUE(value.valid());
    EXPECT_EQ(ContainerShape::decode(value.encode()), value);
    EXPECT_TRUE(value.entails(shape()));
  }
  EXPECT_FALSE(shape().entails(shape(ContainerAccess::Write)));
  EXPECT_FALSE(
      shape(ContainerAccess::Write).entails(shape(ContainerAccess::Release)));
  EXPECT_TRUE(
      shape(ContainerAccess::Release).entails(shape(ContainerAccess::Write)));
  auto other = shape();
  other.object.identity = "unrelated:node";
  EXPECT_FALSE(shape().entails(other));
  other = shape(ContainerAccess::Release);
  other.family = "close_node";
  EXPECT_FALSE(shape(ContainerAccess::Release).entails(other));
}

TEST(ContainerShape, RejectsMalformedLayoutsAndNoncanonicalRecords) {
  const auto encoded = shape().encode();
  for (std::size_t end = 0; end < encoded.size(); ++end)
    EXPECT_FALSE(ContainerShape::decode(encoded.substr(0, end))) << end;
  EXPECT_FALSE(ContainerShape::decode(encoded + "garbage"));
  EXPECT_FALSE(ContainerShape::decode("chain1:00:" + encoded.substr(9)));
  EXPECT_FALSE(ContainerShape::decode(
      std::string(MaxContainerDescriptorBytes + 1, 'x')));
  auto value = shape();
  value.link.offset = 15;
  EXPECT_FALSE(value.valid());
  value = shape();
  value.initialized.push_back(value.initialized.back());
  EXPECT_FALSE(value.valid());
  value = shape();
  value.initialized.clear();
  EXPECT_FALSE(value.valid());
  value = shape();
  value.family = "free";
  EXPECT_FALSE(value.valid());
  value = shape();
  value.link.name = "next\n";
  EXPECT_FALSE(value.valid());
}

TEST(ContainerGraph, NullIsEmptyButUnknownNeverIs) {
  ContainerGraph graph;
  EXPECT_TRUE(graph.prove(ContainerEdge::null(), shape()).complete());
  EXPECT_FALSE(graph.prove({}, shape()).complete());
  EXPECT_FALSE(graph.prove(ContainerEdge::null(), shape(), {}).complete());
  EXPECT_FALSE(graph.prove(ContainerEdge::to(PlaceId{1}), shape()).complete());
}

TEST(ContainerGraph, EndpointIsExclusiveAndCyclesAreRejected) {
  ContainerGraph graph;
  graph.nodes.emplace(PlaceId{1}, node(ContainerEdge::to(PlaceId{2})));
  graph.nodes.emplace(PlaceId{2}, node());
  EXPECT_TRUE(graph.prove(ContainerEdge::to(PlaceId{1}), shape()).complete());
  const auto prefix = graph.prove(ContainerEdge::to(PlaceId{1}), shape(),
                                  ContainerEdge::to(PlaceId{2}));
  ASSERT_TRUE(prefix.complete());
  EXPECT_EQ(prefix.members, (std::set<PlaceId>{PlaceId{1}}));
  graph.nodes.at(PlaceId{2}).next = ContainerEdge::to(PlaceId{1});
  EXPECT_EQ(graph.prove(ContainerEdge::to(PlaceId{1}), shape()).failure,
            ContainerFailure::Cycle);
  EXPECT_TRUE(graph
                  .prove(ContainerEdge::to(PlaceId{1}), shape(),
                         ContainerEdge::to(PlaceId{1}))
                  .complete());
}

TEST(ContainerGraph, EveryNodeMustHaveTheRequiredIndependentEvidence) {
  const auto start = ContainerEdge::to(PlaceId{1});
  ContainerGraph base;
  base.nodes.emplace(PlaceId{1}, node(ContainerEdge::to(PlaceId{2})));
  base.nodes.emplace(PlaceId{2}, node());
  for (const auto id : {PlaceId{1}, PlaceId{2}}) {
    auto graph = base;
    graph.nodes.at(id).live = false;
    EXPECT_FALSE(graph.prove(start, shape()).complete());
    graph = base;
    graph.nodes.at(id).initialized.clear();
    EXPECT_EQ(graph.prove(start, shape()).failure,
              ContainerFailure::Initialization);
    graph = base;
    graph.nodes.at(id).writable = false;
    EXPECT_TRUE(graph.prove(start, shape()).complete());
    EXPECT_EQ(graph.prove(start, shape(ContainerAccess::Write)).failure,
              ContainerFailure::Writable);
    graph = base;
    graph.nodes.at(id).allocationBase = false;
    EXPECT_TRUE(graph.prove(start, shape()).complete());
    EXPECT_EQ(graph.prove(start, shape(ContainerAccess::Release)).failure,
              ContainerFailure::Release);
    graph = base;
    graph.nodes.at(id).family = "other";
    EXPECT_EQ(graph.prove(start, shape(ContainerAccess::Release)).failure,
              ContainerFailure::Release);
    graph = base;
    graph.nodes.at(id).object.identity = "different";
    EXPECT_EQ(graph.prove(start, shape()).failure, ContainerFailure::View);
    graph = base;
    graph.nodes.at(id).next = {};
    EXPECT_FALSE(graph.prove(start, shape()).complete());
  }
}

TEST(ContainerFacts,
     JoinRequiresEveryIncomingProofAndUnionsInvalidationMembers) {
  const auto descriptor = shape();
  ContainerFacts left;
  ContainerFacts right;
  ASSERT_TRUE(
      left.set(PlaceId{1},
               {.shape = descriptor, .members = {PlaceId{2}}, .inputs = {}}));
  ASSERT_TRUE(
      right.set(PlaceId{1},
                {.shape = descriptor, .members = {PlaceId{3}}, .inputs = {}}));
  EXPECT_TRUE(left.join(right));
  ASSERT_NE(left.find(PlaceId{1}), nullptr);
  EXPECT_EQ(left.find(PlaceId{1})->members,
            (std::set<PlaceId>{PlaceId{2}, PlaceId{3}}));
  auto lost = left;
  lost.invalidate(PlaceId{3});
  EXPECT_EQ(lost.find(PlaceId{1}), nullptr);
  EXPECT_FALSE(left.join(left));
  ContainerFacts unknown;
  EXPECT_TRUE(left.join(unknown));
  EXPECT_EQ(left.find(PlaceId{1}), nullptr);
}

TEST(ContainerFacts, EmptyBranchDoesNotRequireOrFabricateStorage) {
  ContainerFacts left;
  ContainerFacts right;
  ASSERT_TRUE(
      left.set(PlaceId{1},
               {.shape = shape(), .members = {}, .inputs = {}, .empty = true}));
  ASSERT_TRUE(right.set(PlaceId{1}, {.shape = shape(ContainerAccess::Release),
                                     .members = {PlaceId{2}},
                                     .inputs = {}}));
  left.join(right);
  ASSERT_NE(left.find(PlaceId{1}), nullptr);
  EXPECT_FALSE(left.find(PlaceId{1})->empty);
  EXPECT_TRUE(left.find(PlaceId{1})->entails(shape(ContainerAccess::Release)));
}

TEST(ContainerShape, CheckedTransportKeepsTheEntirePredicate) {
  CheckedContract contract;
  contract.computed = true;
  contract.signature = "void (struct node *)";
  contract.require({.kind = CheckedRequirementKind::Container,
                    .path = SummaryPath::param(0),
                    .other = {},
                    .begin = {},
                    .end = {},
                    .family = shape(ContainerAccess::Release).encode()});
  const auto encoded = printCheckedContract(contract, {});
  const auto decoded = parseCheckedContract(encoded, {});
  ASSERT_TRUE(decoded);
  EXPECT_EQ(*decoded, contract);
  contract.requirements.clear();
  contract.require({.kind = CheckedRequirementKind::Container,
                    .path = SummaryPath::param(0),
                    .other = {},
                    .begin = {},
                    .end = {},
                    .family = "malformed"});
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
}

// Independent finite-state oracle: follow integer edges in an ordinary graph.
// It does not invoke ContainerGraph, ContainerShape or the abstract transfer.
static bool concreteChain(const std::vector<int> &successors,
                          unsigned initialized, unsigned live, int cursor) {
  std::set<int> seen;
  while (cursor != -1) {
    if (cursor < 0 || static_cast<std::size_t>(cursor) >= successors.size() ||
        !seen.insert(cursor).second ||
        (initialized & (1U << static_cast<unsigned>(cursor))) == 0 ||
        (live & (1U << static_cast<unsigned>(cursor))) == 0)
      return false;
    cursor = successors[static_cast<std::size_t>(cursor)];
  }
  return true;
}

TEST(ContainerGraph, ExhaustiveSmallHeapOracleNeverAcceptsAnInvalidChain) {
  constexpr unsigned Nodes = 3;
  constexpr unsigned Choices = Nodes + 2;
  unsigned checked = 0;
  for (unsigned encoded = 0; encoded < Choices * Choices * Choices; ++encoded) {
    unsigned remaining = encoded;
    std::vector<int> links;
    for (unsigned i = 0; i < Nodes; ++i) {
      links.push_back(static_cast<int>(remaining % Choices) - 1);
      remaining /= Choices;
    }
    for (unsigned initialized = 0; initialized < (1U << Nodes); ++initialized)
      for (unsigned live = 0; live < (1U << Nodes); ++live) {
        ContainerGraph graph;
        for (unsigned i = 0; i < Nodes; ++i) {
          auto value = node();
          value.live = (live & (1U << i)) != 0;
          if ((initialized & (1U << i)) == 0)
            value.initialized.clear();
          value.next = links[i] == -1
                           ? ContainerEdge::null()
                           : ContainerEdge::to(PlaceId{
                                 static_cast<std::uint32_t>(links[i] + 1)});
          graph.nodes.emplace(PlaceId{i + 1}, std::move(value));
        }
        for (unsigned start = 0; start < Nodes; ++start) {
          const auto proof =
              graph.prove(ContainerEdge::to(PlaceId{start + 1}), shape());
          EXPECT_EQ(proof.complete(), concreteChain(links, initialized, live,
                                                    static_cast<int>(start)))
              << encoded << ':' << initialized << ':' << live << ':' << start;
          ++checked;
        }
      }
  }
  EXPECT_EQ(checked, 24000U);
}

TEST(ContainerShape, TerminalHeadsAreAnAdditionalRequirement) {
  auto single = shape();
  single.terminal = true;
  EXPECT_TRUE(single.entails(shape()));
  EXPECT_FALSE(shape().entails(single));
  EXPECT_EQ(ContainerShape::decode(single.encode()), single);
  ContainerGraph graph;
  graph.nodes.emplace(PlaceId{1}, node(ContainerEdge::to(PlaceId{2})));
  graph.nodes.emplace(PlaceId{2}, node());
  EXPECT_FALSE(graph.prove(ContainerEdge::to(PlaceId{1}), single).complete());
  EXPECT_TRUE(graph.prove(ContainerEdge::to(PlaceId{2}), single).complete());
  EXPECT_TRUE(graph.prove(ContainerEdge::null(), single).complete());
}

TEST(ContainerFacts,
     JoiningTerminalAndLongerChainsKeepsOnlyTheCommonPredicate) {
  auto single = shape();
  single.terminal = true;
  ContainerFacts left;
  ContainerFacts right;
  left.set(PlaceId{1},
           {.shape = single, .members = {PlaceId{2}}, .inputs = {}});
  right.set(
      PlaceId{1},
      {.shape = shape(), .members = {PlaceId{2}, PlaceId{3}}, .inputs = {}});
  ASSERT_TRUE(left.join(right));
  ASSERT_NE(left.find(PlaceId{1}), nullptr);
  EXPECT_FALSE(left.find(PlaceId{1})->shape.terminal);
  EXPECT_EQ(left.find(PlaceId{1})->members.size(), 2U);
}

TEST(ContainerFacts, ForgettingAProofDoesNotPretendTheHolderChangedValue) {
  ContainerFacts facts;
  facts.set(PlaceId{1},
            {.shape = shape(), .members = {PlaceId{10}}, .inputs = {}});
  facts.set(PlaceId{2}, {.shape = shape(),
                         .members = {PlaceId{10}},
                         .inputs = {},
                         .tailOf = PlaceId{1}});
  facts.block(PlaceId{1});
  ASSERT_NE(facts.find(PlaceId{2}), nullptr);
  EXPECT_EQ(facts.find(PlaceId{2})->tailOf, PlaceId{1});
  EXPECT_TRUE(facts.blocked(PlaceId{1}));
  facts.replace(PlaceId{1});
  EXPECT_FALSE(facts.find(PlaceId{2})->tailOf);
}

TEST(ContainerFacts, SeparationDoesNotFollowAnOverwrittenPointer) {
  ContainerFacts facts;
  facts.set(PlaceId{1},
            {.shape = shape(), .members = {PlaceId{10}}, .inputs = {}});
  facts.set(PlaceId{2},
            {.shape = shape(), .members = {PlaceId{20}}, .inputs = {}});
  facts.separate(PlaceId{1}, PlaceId{2});
  EXPECT_TRUE(facts.separated(PlaceId{1}, PlaceId{2}));
  facts.replace(PlaceId{1});
  EXPECT_FALSE(facts.separated(PlaceId{1}, PlaceId{2}));
  facts.clear();
  EXPECT_TRUE(facts.blocked(PlaceId{123}));
}

TEST(ContainerFacts, LimitsCannotReopenAnEntryAssumption) {
  ContainerFacts facts;
  for (std::uint32_t i = 0; i < MaxContainerFacts; ++i)
    ASSERT_TRUE(facts.set(PlaceId{i + 1},
                          {.shape = shape(), .members = {}, .inputs = {}}));
  EXPECT_FALSE(facts.set(PlaceId{1000},
                         {.shape = shape(), .members = {}, .inputs = {}}));
  EXPECT_TRUE(facts.limited());
  EXPECT_TRUE(facts.blocked(PlaceId{1000}));
}

static ContainerShape payloadShape() {
  auto result = shape(ContainerAccess::Release);
  result.object.bytes = 24;
  result.initialized = {{.name = "data", .offset = 0, .bytes = 8},
                        result.link,
                        {.name = "value", .offset = 16, .bytes = 4}};
  result.payloads = {{.field = result.initialized.front(), .family = "free"}};
  return result;
}

TEST(ContainerGraph,
     PayloadsMustBeOwnedSeparatedAndOutsideTheSurvivingEndpoint) {
  const auto descriptor = payloadShape();
  auto first = node(ContainerEdge::to(PlaceId{2}));
  first.object = descriptor.object;
  first.initialized = {descriptor.initialized.begin(),
                       descriptor.initialized.end()};
  first.payloads["data"] = ContainerEdge::to(PlaceId{3});
  auto second = first;
  second.next = ContainerEdge::null();
  second.payloads["data"] = ContainerEdge::null();
  ContainerGraph graph;
  graph.nodes = {
      {PlaceId{1}, first}, {PlaceId{2}, second}, {PlaceId{3}, node()}};
  ASSERT_TRUE(
      graph.prove(ContainerEdge::to(PlaceId{1}), descriptor).complete());
  auto changed = graph;
  changed.nodes.at(PlaceId{2}).payloads["data"] = ContainerEdge::to(PlaceId{3});
  EXPECT_EQ(changed.prove(ContainerEdge::to(PlaceId{1}), descriptor).failure,
            ContainerFailure::Overlap);
  changed = graph;
  changed.nodes.at(PlaceId{3}).allocationBase = false;
  EXPECT_EQ(changed.prove(ContainerEdge::to(PlaceId{1}), descriptor).failure,
            ContainerFailure::Release);
  changed = graph;
  changed.nodes.at(PlaceId{3}).live = false;
  EXPECT_EQ(changed.prove(ContainerEdge::to(PlaceId{1}), descriptor).failure,
            ContainerFailure::Release);
  changed = graph;
  changed.nodes.at(PlaceId{1}).payloads["data"] = ContainerEdge::to(PlaceId{2});
  EXPECT_EQ(changed.prove(ContainerEdge::to(PlaceId{1}), descriptor).failure,
            ContainerFailure::Overlap);
  EXPECT_EQ(changed
                .prove(ContainerEdge::to(PlaceId{1}), descriptor,
                       ContainerEdge::to(PlaceId{2}))
                .failure,
            ContainerFailure::Overlap);
}

TEST(ContainerFacts, AConsumedPayloadCannotBeReleasedAgain) {
  auto descriptor = payloadShape();
  ContainerFact fact{
      .shape = descriptor, .members = {PlaceId{1}}, .inputs = {}};
  ASSERT_TRUE(fact.entails(descriptor));
  fact.releasedPayloads.insert("data");
  EXPECT_FALSE(fact.entails(descriptor));
  descriptor.access = ContainerAccess::Read;
  descriptor.family.clear();
  descriptor.payloads.clear();
  EXPECT_TRUE(fact.entails(descriptor));
}

TEST(ContainerGraph, PayloadCertificatesMatchConcreteReleaseSequences) {
  const auto descriptor = payloadShape();
  unsigned checked = 0;
  // Objects 1 and 2 are linked nodes, 3 and 4 are payload allocations;
  // 0 is null and 5 is unknown. The reference executes actual release order
  // using integer object identities. It does not query the abstract domain.
  for (unsigned first = 0; first < 6; ++first)
    for (unsigned second = 0; second < 6; ++second)
      for (unsigned live = 0; live < 16; ++live)
        for (unsigned owned = 0; owned < 16; ++owned) {
          unsigned remaining = live;
          const auto release = [&](unsigned object) {
            if (object == 0)
              return true;
            if (object > 4 || (remaining & (1U << (object - 1))) == 0 ||
                (owned & (1U << (object - 1))) == 0)
              return false;
            remaining &= ~(1U << (object - 1));
            return true;
          };
          const bool expected = (remaining & 1U) != 0 && release(first) &&
                                release(1) && (remaining & 2U) != 0 &&
                                release(second) && release(2);
          ContainerGraph graph;
          for (unsigned object = 1; object <= 4; ++object) {
            auto value = node();
            value.live = (live & (1U << (object - 1))) != 0;
            value.allocationBase = (owned & (1U << (object - 1))) != 0;
            if (object <= 2) {
              value.object = descriptor.object;
              value.initialized = {descriptor.initialized.begin(),
                                   descriptor.initialized.end()};
              value.next = object == 1 ? ContainerEdge::to(PlaceId{2})
                                       : ContainerEdge::null();
              const auto payload = object == 1 ? first : second;
              value.payloads["data"] = payload
                                           ? ContainerEdge::to(PlaceId{payload})
                                           : ContainerEdge::null();
            }
            graph.nodes.emplace(PlaceId{object}, std::move(value));
          }
          EXPECT_EQ(
              graph.prove(ContainerEdge::to(PlaceId{1}), descriptor).complete(),
              expected)
              << first << ':' << second << ':' << live << ':' << owned;
          ++checked;
        }
  EXPECT_EQ(checked, 9216U);
}

TEST(ContainerShape, DerivedOutputsTransportEverySourcePremise) {
  CheckedContract contract;
  contract.computed = true;
  for (unsigned i = 0; i < 3; ++i)
    contract.require({.kind = CheckedRequirementKind::Container,
                      .path = SummaryPath::param(i),
                      .other = {},
                      .family = shape().encode()});
  contract.establish({.kind = CheckedRequirementKind::ContainerDerived,
                      .path = SummaryPath::result(),
                      .other = SummaryPath::param(0),
                      .begin = PathAffine::ofPath(SummaryPath::param(1)),
                      .end = PathAffine::ofPath(SummaryPath::param(2)),
                      .family = shape().encode()});
  EXPECT_EQ(parseCheckedContract(printCheckedContract(contract, {}), {}),
            contract);
  auto bad = contract;
  bad.requirements.clear();
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
  bad = contract;
  auto post = *bad.establishes.begin();
  post.begin = PathAffine::ofPath(SummaryPath::param(1), 2);
  bad.establishes = {post};
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
  post = *contract.establishes.begin();
  bad.requirements = {post};
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
  bad.requirements.clear();
  post.kind = CheckedRequirementKind::ContainerFresh;
  post.begin = post.end = {};
  bad.establishes = {post};
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(bad, {}), {}));
}

TEST(ContainerShape, TailOutputsRequireAnEntryShape) {
  CheckedContract contract;
  contract.computed = true;
  contract.establish({.kind = CheckedRequirementKind::ContainerTail,
                      .path = SummaryPath::result(),
                      .other = SummaryPath::param(0),
                      .family = shape().encode()});
  EXPECT_FALSE(parseCheckedContract(printCheckedContract(contract, {}), {}));
  contract.require({.kind = CheckedRequirementKind::Container,
                    .path = SummaryPath::param(0),
                    .other = {},
                    .family = shape().encode()});
  EXPECT_EQ(parseCheckedContract(printCheckedContract(contract, {}), {}),
            contract);
}

TEST(ContainerShape, JoiningDerivedOutputsRetainsEveryAlternativeSource) {
  CheckedRequirement first{.kind = CheckedRequirementKind::ContainerDerived,
                           .path = SummaryPath::result(),
                           .other = SummaryPath::param(0),
                           .family = shape().encode()};
  auto second = first;
  second.other = SummaryPath::param(1);
  auto joined = joinContainerOutput(first, second);
  ASSERT_TRUE(joined);
  EXPECT_EQ(joined->other, SummaryPath::param(0));
  EXPECT_EQ(joined->begin.path, SummaryPath::param(1));
  second.other = SummaryPath::param(2);
  joined = joinContainerOutput(*joined, second);
  ASSERT_TRUE(joined);
  EXPECT_EQ(joined->end.path, SummaryPath::param(2));
  second.other = SummaryPath::param(3);
  joined = joinContainerOutput(*joined, second);
  ASSERT_TRUE(joined);
  EXPECT_EQ(joined->kind, CheckedRequirementKind::Container);
}

TEST(ContainerFacts, QuantifiedLifetimeInvalidationSurvivesEitherJoinOrder) {
  const PlaceId alias{1};
  SafetyState live;
  live.pointers.insert(alias);
  SafetyState consumed;
  consumed.invalidatedPointers.insert(alias);
  auto first = live;
  auto second = consumed;
  EXPECT_TRUE(first.join(consumed));
  second.join(live);
  EXPECT_EQ(first.invalidatedPointers, second.invalidatedPointers);
  EXPECT_TRUE(first.invalidatedPointers.contains(alias));
  EXPECT_FALSE(first.pointers.contains(alias));
  EXPECT_FALSE(first.join(first));
  first.forget(alias);
  EXPECT_FALSE(first.invalidatedPointers.contains(alias));
}

} // namespace weavec::core
