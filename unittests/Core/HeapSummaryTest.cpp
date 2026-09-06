//===- HeapSummaryTest.cpp - Heap graphs and identities (RFC 0013) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/AnalysisState.h"
#include "weavec/Core/SummaryIO.h"

#include <gtest/gtest.h>

namespace weavec::core {

static HeapDescription graphWithAliases() {
  HeapDescription graph;
  const auto root = SummaryPath::result();
  ValueSource child =
      ValueSource::freshAt("free", {}, PathAffine::ofConstant(8));
  child.stringLength = PathAffine::ofConstant(3);
  graph.addField(Store{.dest = root.deref().field("a"), .value = child});
  ValueSource shared = ValueSource::copy(root.deref().field("a"));
  shared.post = true;
  graph.addField(Store{.dest = root.deref().field("b"), .value = shared});
  ValueSource self = ValueSource::copy(root);
  self.post = true;
  graph.addField(Store{.dest = root.deref().field("self"), .value = self});
  graph.addField(Store{.dest = root.deref().field("input"),
                       .value = ValueSource::copy(SummaryPath::param(0))});
  return graph;
}

TEST(HeapSummary, RoundTripsFreshSharedSelfAndInputReferences) {
  FunctionSummary summary;
  summary.addReturn(ValueSource::fresh("free"));
  summary.heap[SummaryPath::result()] = graphWithAliases();
  EXPECT_TRUE(summary.heap.begin()->second.valid());
  const GlobalNamer names = [](std::uint32_t id) { return std::to_string(id); };
  const GlobalResolver resolve = [](std::string_view) { return 0U; };
  const std::string text = printSummary(summary, names);
  EXPECT_NE(text.find("copy-post result *.a"), std::string::npos);
  EXPECT_NE(text.find("length 3"), std::string::npos);
  std::string error;
  const auto read = parseSummary(text, resolve, &error);
  ASSERT_TRUE(read) << error << '\n' << text;
  EXPECT_EQ(*read, summary);
  EXPECT_EQ(printSummary(*read, names), text);
}

TEST(HeapSummary, MissingFieldsContributeUnknownAndJoinIsIdempotent) {
  auto graph = graphWithAliases();
  graph.join(HeapDescription{});
  const auto root = SummaryPath::result();
  EXPECT_TRUE(graph.fields.contains(
      Store{.dest = root.deref().field("a"), .value = ValueSource::unknown()}));
  const auto before = graph;
  graph.join(graph);
  EXPECT_EQ(graph, before);
  graph.join(before);
  EXPECT_EQ(graph, before);
  HeapDescription missing;
  missing.join(graphWithAliases());
  EXPECT_EQ(missing, graph);
}

TEST(HeapSummary, NullResultDoesNotWeakenPointeeFields) {
  FunctionSummary fresh;
  fresh.addReturn(ValueSource::fresh("free"));
  fresh.heap[SummaryPath::result()] = graphWithAliases();
  FunctionSummary null;
  null.addReturn(ValueSource::null());
  const auto graph = fresh.heap;
  fresh.join(null);
  EXPECT_EQ(fresh.heap, graph);
  null.join(fresh);
  EXPECT_EQ(null.heap, graph);
  FunctionSummary unknown;
  unknown.addReturn(ValueSource::unknown());
  fresh.join(unknown);
  EXPECT_GT(fresh.heap.begin()->second.fields.size(),
            graph.begin()->second.fields.size());
}

TEST(HeapSummary,
     RemappingGlobalsPreservesGraphIdentityAndWeakensMissingInputs) {
  FunctionSummary original;
  auto &graph = original.heap[SummaryPath::global(1)];
  ValueSource value = ValueSource::freshAt(
      "free", {}, PathAffine::ofPath(SummaryPath::global(2)));
  value.stringLength = PathAffine::ofPath(SummaryPath::global(3));
  graph.addField(Store{.dest = SummaryPath::result().deref().field("data"),
                       .value = value});
  graph.addField(Store{.dest = SummaryPath::result().deref().field("input"),
                       .value = ValueSource::copy(SummaryPath::global(4))});
  graph.incomplete = true;
  const auto mapped = remapGlobals(
      original, [](std::uint32_t id) -> std::optional<std::uint32_t> {
        return id == 3 || id == 4 ? std::nullopt : std::optional(id + 10);
      });
  ASSERT_TRUE(mapped.heap.contains(SummaryPath::global(11)));
  const auto &output = mapped.heap.at(SummaryPath::global(11));
  EXPECT_TRUE(output.incomplete);
  EXPECT_TRUE(output.valid());
  for (const Store &field : output.fields) {
    EXPECT_FALSE(field.value.stringLength);
    if (field.value.isFresh())
      EXPECT_EQ(field.value.extent,
                PathAffine::ofPath(SummaryPath::global(12)));
    else
      EXPECT_EQ(field.value.kind, ValueSource::Kind::Unknown);
  }
}

TEST(HeapSummary, RejectsMalformedAndUnresolvedGraphs) {
  const GlobalResolver resolve = [](std::string_view) { return 0U; };
  for (const std::string body :
       {"heap result complete\nheap-field result at param 0 fresh",
        "heap result complete\nheap-field result at result *.a copy-post "
        "result *.missing",
        "heap result complete\nheap-field result at result *.a copy-post param "
        "0",
        "heap result complete\nheap-field result at result *.a fresh length 1 "
        "unterminated",
        "heap result nonsense", "heap-field result at result *.a fresh",
        "heap param 0 complete", "return copy-post result",
        "store param 0 * copy-post result"}) {
    std::string error;
    EXPECT_FALSE(parseSummary("summary\n" + body + "\nend\n", resolve, &error))
        << body;
    EXPECT_FALSE(error.empty()) << body;
  }
}

TEST(HeapSummary, IncomingPointerIdentityJoinsOnlyByAgreement) {
  const PlaceId local{1};
  const auto value = ValueSource::copy(SummaryPath::param(0).deref());
  AnalysisState left;
  left.incoming[local] = value;
  AnalysisState same = left;
  EXPECT_FALSE(left.join(same));
  same.incoming[local] = ValueSource::copy(SummaryPath::param(1));
  EXPECT_TRUE(left.join(same));
  EXPECT_TRUE(left.incoming.empty());
  left.incoming[local] = value;
  left.forget(local);
  EXPECT_TRUE(left.incoming.empty());
}

TEST(HeapSummary, ProjectionBoundsAreFiniteAndRoundTripAfterWidening) {
  HeapDescription graph;
  const auto cell = SummaryPath::result().deref().field("data");
  for (std::size_t i = 0; i <= MaxHeapAlternatives; ++i)
    graph.addField(Store{
        .dest = cell,
        .value = ValueSource::freshAt(
            "free", {}, PathAffine::ofConstant(static_cast<std::int64_t>(i)))});
  EXPECT_TRUE(graph.incomplete);
  ASSERT_EQ(graph.fields.size(), 1U);
  EXPECT_EQ(graph.fields.begin()->value.kind, ValueSource::Kind::Unknown);
  graph.addField(Store{.dest = cell, .value = ValueSource::raw()});
  EXPECT_EQ(graph.fields.size(), 1U);
  FunctionSummary summary;
  summary.heap[SummaryPath::result()] = graph;
  const auto text = printSummary(summary, [](std::uint32_t) { return "g"; });
  EXPECT_EQ(parseSummary(text, [](std::string_view) { return 0U; }), summary);
  auto deep = SummaryPath::result();
  for (std::size_t i = 0; i <= MaxHeapPathDepth; ++i)
    deep = deep.deref();
  graph.addField(Store{.dest = deep, .value = ValueSource::null()});
  EXPECT_EQ(graph.fields.size(), 1U);
  HeapDescription large;
  for (std::size_t i = 0; i <= MaxHeapFields; ++i)
    large.addField(Store{.dest = SummaryPath::result().field(std::to_string(i)),
                         .value = ValueSource::null()});
  EXPECT_EQ(large.fields.size(), MaxHeapFields);
  EXPECT_TRUE(large.incomplete);
  EXPECT_TRUE(large.valid());
}

TEST(HeapSummary, ExternalPostReferencesMustNameDefinedOutputCells) {
  const GlobalResolver resolve = [](std::string_view) { return 0U; };
  const std::string prefix =
      "summary\nheap param 0 * complete\nheap-field param 0 * at result "
      "fresh\nheap param 1 * complete\nheap-field param 1 * at result "
      "copy-post param 0 *\nreturn copy-post param 1 *\nend\n";
  // A chain of output-only references is not canonical: imports cannot
  // depend on their iteration order.
  EXPECT_FALSE(parseSummary(prefix, resolve));
  std::string canonical = prefix;
  canonical.replace(canonical.find("return copy-post param 1"),
                    std::string("return copy-post param 1").size(),
                    "return copy-post param 0");
  EXPECT_TRUE(parseSummary(canonical, resolve));
  EXPECT_FALSE(
      parseSummary("summary\nheap result complete\nheap-field result at result "
                   "*.data copy-post param 0 *.missing\nend\n",
                   resolve));
}

TEST(HeapSummary, DefiniteAliasesIntersectAtControlFlowJoins) {
  AnalysisState left;
  left.aliases.unite(PlaceId{1}, PlaceId{2});
  left.definiteAliases.unite(PlaceId{1}, PlaceId{2});
  AnalysisState right;
  left.join(right);
  EXPECT_TRUE(left.aliases.isExact(PlaceId{1}, PlaceId{2}));
  EXPECT_FALSE(left.definiteAliases.isExact(PlaceId{1}, PlaceId{2}));
}

TEST(HeapSummary, NullContainerHasNoContradictoryChildExtent) {
  PlaceTable places;
  const auto root = places.create("p");
  const auto field = places.field(places.deref(root), "data");
  AnalysisState allocated;
  SpatialRecord fact;
  fact.extent = Affine::ofConstant(4);
  allocated.spatial.set(field, fact);
  AnalysisState null;
  null.resources.markNull(root);
  AnalysisState forward = allocated;
  forward.join(null, &places);
  ASSERT_TRUE(forward.spatial.recordOf(field));
  EXPECT_EQ(forward.spatial.recordOf(field)->extent, Affine::ofConstant(4));
  EXPECT_FALSE(forward.join(null, &places));
  null.join(allocated, &places);
  EXPECT_EQ(null.spatial, forward.spatial);
  AnalysisState unknown;
  forward.join(unknown, &places);
  EXPECT_FALSE(forward.spatial.has(field));
}

TEST(HeapSummary, PublicationGuardsSurviveAnUntouchedPredecessor) {
  AnalysisState written;
  const PlaceId p{1};
  PathGuard when;
  when.require(SummaryPath::global(0), ValueFact::of(Outcome::Null));
  written.heapWriteGuards[p] = when;
  written.definiteHeapWrites.insert(p);
  AnalysisState untouched;
  written.join(untouched);
  EXPECT_EQ(written.heapWriteGuards.at(p), when);
  EXPECT_FALSE(written.definiteHeapWrites.contains(p));
  EXPECT_FALSE(written.join(untouched));
  written.forget(p);
  EXPECT_FALSE(written.heapWriteGuards.contains(p));
}

TEST(HeapSummary, LocalObjectsRequireEveryNonNullAlternative) {
  const PlaceId p{1};
  AnalysisState fresh;
  fresh.heapLocalObjects.insert(p);
  AnalysisState null;
  null.resources.markNull(p);
  auto forward = fresh;
  forward.join(null);
  EXPECT_TRUE(forward.heapLocalObjects.contains(p));
  null.join(fresh);
  EXPECT_EQ(null.heapLocalObjects, forward.heapLocalObjects);
  EXPECT_FALSE(forward.join(fresh));
  forward.join(AnalysisState{});
  EXPECT_TRUE(forward.heapLocalObjects.empty());
  fresh.forget(p);
  EXPECT_TRUE(fresh.heapLocalObjects.empty());
}

} // namespace weavec::core
