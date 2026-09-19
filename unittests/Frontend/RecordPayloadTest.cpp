//===- RecordPayloadTest.cpp - Tests for the unit record's payload --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/RecordPayload.h"

#include "weavec/Core/SummaryIO.h"
#include "weavec/Frontend/UnitRecord.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>

namespace weavec::frontend::record {
namespace {

using core::FunctionSummary;
using core::PlaceEffect;
using core::SummaryPath;
using core::ValueSource;

core::SourceLocation at(std::string file, std::uint32_t line,
                        std::uint32_t column) {
  return core::SourceLocation{
      .file = std::move(file), .line = line, .column = column, .opaque = 0};
}

/// A payload with something in every collection of the table and no null
/// where a value is allowed, so that every key the table describes appears.
Payload fullPayload() {
  Payload payload;
  analysis::UnitExports &exports = payload.exports;
  exports.source = "src/node.c";
  const std::uint32_t cache = exports.globals.idFor("g_cache");
  FunctionSummary freeSummary;
  freeSummary.addEffect(SummaryPath::param(0), PlaceEffect{.freed = true});
  freeSummary.addEffect(SummaryPath::global(cache), PlaceEffect{.freed = true});
  analysis::ExportedFunction &node = exports.functions["node_free"];
  node.summary.assign(freeSummary);
  node.typeKey = "void (struct node *)";
  node.addressTaken = true;
  node.acceptsCallbacks = true;
  const core::CallbackBindings bindings{
      {SummaryPath::param(0), core::CallTargets::function("drop")}};
  node.specializations[bindings].assign(freeSummary);
  core::CallContext context;
  context.facts[SummaryPath::param(1)] = core::ValueFact::ofConstant(3);
  node.memorySpecializations[context].assign(freeSummary);
  exports.memoryRequests["node_free"].insert(context);
  exports.callbackRequests["node_free"].insert(bindings);
  exports.callbackGlobals["hook"] = core::CallTargets::function("drop");
  exports.imports = {"blob_open"};
  exports.indirectTypes = {"void (void *)"};
  exports.unknownCallees = {"blob_open"};
  exports.unknownIndirectTypes = {"int (struct opaque *)"};
  exports.countFields = {"struct node.rc"};
  exports.sizedFields.witnesses = {analysis::SizedFieldWitness{
      .field = "struct vec.items",
      .count = "struct vec.cap",
      .scale = 4,
      .productType = core::IntegerType{.width = 64, .isSigned = false}}};
  exports.sizedFields.unsizedFields = {"struct node.next"};
  exports.sizedFields.unsizedPairs = {analysis::UnsizedPair{
      .field = "struct vec.items", .count = "struct vec.n"}};
  exports.sizedFieldLoads = {"struct node.children"};
  core::InterfaceNode integer;
  integer.kind = core::InterfaceKind::Integer;
  integer.bytes = 4;
  integer.alignment = 4;
  integer.name = "unsigned int";
  exports.globalInterfaces["g_count"] = core::InterfaceType{{integer}};
  exports.objectInterfaces["view"] = core::InterfaceType{{integer}};

  InterfaceFacts &facts = payload.facts;
  facts.functions["node_free"] =
      FunctionInterface{.params = {"default single nullable"},
                        .result = "inferred single nonnull",
                        .reliesOnSingle = {0},
                        .requirements = {ExportedRequirement{
                            .param = 0,
                            .kind = "counted(param 1 scale 1 plus 0) nonnull",
                            .guard = "0 < param 1 scale 1 plus 0"}},
                        .location = at("src/node.c", 13, 6)};
  facts.globals["g_cache"] =
      GlobalInterface{.typeKey = "char *", .kind = "inferred single nullable"};
  facts.imports["blob_open"] = ImportInterface{
      .declared = DeclaredInterface{.params = {DeclaredParam{
                                        .name = "path",
                                        .kind = "unknown nonnull",
                                        .ownership = "WEAVEC_BORROWED"}},
                                    .result = "single nullable",
                                    .ownership = "WEAVEC_OWNED"},
      .location = at("src/blob.h", 3, 14),
      .calls = {ImportCall{.function = "main", .site = 2, .args = {false}}}};
  facts.slots = SlotFacts{
      .rows = {core::SlotRow{.slot = core::SlotKey::field("struct ops", "cb"),
                             .targets = {"drop"},
                             .sources = {core::SlotKey::global("hook")},
                             .open = "'hook' may be stored elsewhere"}},
      .unit = "src/node.c",
      .defined = {"node_free"},
      .exported = {"node_free"},
      .confinedRecords = {"src/node.c:struct local"},
      .escapedStatics = {"src/node.c:table"}};
  facts.slotKinds = {SlotKindRow{.slot = "field struct node next",
                                 .kind = "unknown nullable",
                                 .demotedBy = {at("src/node.c", 40, 11)}}};
  facts.invariants = {InvariantRow{.record = "struct vec",
                                   .field = "items",
                                   .templ = "count(items) == cap + 0",
                                   .verdict = core::Verdict::Violated,
                                   .relied = true,
                                   .store = at("src/vec.c", 20, 5)}};
  facts.boundaries = {
      analysis::BoundaryRow{.unit = {},
                            .function = "remember",
                            .site = 1,
                            .reason = core::UnresolvedReason::DanglingEscape,
                            .placeClass = "g_cache"}};
  facts.allocator = "malloc";
  facts.loweredAllocations = 3;
  payload.sites = {FunctionRows{
      .function = "node_free",
      .file = "src/node.c",
      .line = 13,
      .linkage = core::Linkage::External,
      .rows = {SiteRow{
          .ordinal = 0,
          .kind = core::SiteKind::Deref,
          .line = 14,
          .column = 3,
          .facets = {
              core::FacetDecision::checked(), core::FacetDecision::proven(),
              core::FacetDecision::unresolvedFor(
                  core::UnresolvedReason::UnknownCallee),
              core::FacetDecision::trustedFor(core::TrustReason::Unsafe)}}}}};
  payload.reported = {ReportedDiagnostic{
      .id = "use-after-free", .file = "src/node.c", .line = 17, .column = 10}};
  payload.a5 =
      core::A5Counts{.nonLoweredAllocations = 2, .bypassedDeclarations = 1};
  return payload;
}

std::vector<std::string> payloadPaths() {
  std::vector<std::string> paths;
  for (const std::string &path : schemaPaths())
    if (llvm::StringRef(path).starts_with("payload"))
      paths.push_back(path);
  return paths;
}

/// Through the bytes of a record and back, as the link step reads it.
std::optional<Payload> throughRecord(const Payload &payload,
                                     std::string &error) {
  UnitRecord unit;
  unit.header.source = payload.exports.source;
  unit.payload = toJson(payload);
  const std::optional<std::string> bytes = encode(unit, &error);
  if (!bytes)
    return std::nullopt;
  const std::optional<UnitRecord> decoded = decode(*bytes, error);
  if (!decoded)
    return std::nullopt;
  return payloadFromJson(decoded->payload, decoded->header.source, error);
}

} // namespace

TEST(RecordPayload, TheEncoderEmitsExactlyTheTableKeys) {
  const llvm::json::Value json = toJson(fullPayload());
  EXPECT_FALSE(validate(json, payloadSchema(), "payload"));
  EXPECT_EQ(valuePaths(json, "payload"), payloadPaths());
}

TEST(RecordPayload, EverythingRoundTrips) {
  const Payload original = fullPayload();
  std::string error;
  const std::optional<Payload> read = throughRecord(original, error);
  ASSERT_TRUE(read) << error;
  EXPECT_EQ(read->exports.source, original.exports.source);
  EXPECT_TRUE(read->exports.sameSummariesAs(original.exports));
  EXPECT_EQ(read->exports.globals, original.exports.globals);
  EXPECT_EQ(read->exports.functions, original.exports.functions);
  EXPECT_EQ(read->exports.imports, original.exports.imports);
  EXPECT_EQ(read->exports.indirectTypes, original.exports.indirectTypes);
  EXPECT_EQ(read->exports.unknownCallees, original.exports.unknownCallees);
  EXPECT_EQ(read->exports.unknownIndirectTypes,
            original.exports.unknownIndirectTypes);
  EXPECT_EQ(read->exports.memoryRequests, original.exports.memoryRequests);
  EXPECT_EQ(read->exports.callbackRequests, original.exports.callbackRequests);
  EXPECT_EQ(read->exports.callbackGlobals, original.exports.callbackGlobals);
  EXPECT_EQ(read->exports.countFields, original.exports.countFields);
  EXPECT_EQ(read->exports.sizedFields, original.exports.sizedFields);
  EXPECT_EQ(read->exports.sizedFieldLoads, original.exports.sizedFieldLoads);
  EXPECT_EQ(read->exports.globalInterfaces, original.exports.globalInterfaces);
  EXPECT_EQ(read->exports.objectInterfaces, original.exports.objectInterfaces);
  EXPECT_EQ(read->facts, original.facts);
  EXPECT_EQ(read->sites, original.sites);
  EXPECT_EQ(read->reported, original.reported);
  EXPECT_EQ(read->a5, original.a5);
  // The same payload encodes to the same text.
  EXPECT_EQ(toJson(*read), toJson(original));
}

TEST(RecordPayload, SummariesKeepHeapGraphsArrayRangesAndGlobalOrder) {
  Payload payload;
  payload.exports.source = "src/lib.c";
  // RFC 0022: the global table keeps the producer's order, unused names
  // included, so callback bindings and contexts keep their keys.
  const std::uint32_t unused = payload.exports.globals.idFor("unused");
  const std::uint32_t zHook = payload.exports.globals.idFor("z_hook");
  const std::uint32_t aHook = payload.exports.globals.idFor("a_hook");
  FunctionSummary summary;
  summary.callbackInputs.insert(SummaryPath::global(zHook));
  summary.callbackInputs.insert(SummaryPath::global(aHook));
  auto &graph = summary.heap[SummaryPath::result()];
  auto child =
      ValueSource::freshAt("free", {}, core::PathAffine::ofConstant(4));
  child.stringLength = core::PathAffine::ofConstant(3);
  graph.addField(core::Store{.dest = SummaryPath::result().deref().field("a"),
                             .value = child});
  graph.incomplete = true;
  summary.arrayReleases.insert({.storage = SummaryPath::param(0).deref(),
                                .begin = core::PathAffine::ofConstant(0),
                                .count = core::PathAffine::ofConstant(3),
                                .when = {},
                                .cleared = true,
                                .definite = false});
  summary.addEffect(SummaryPath::param(0).deref().indexed("$1+2"),
                    PlaceEffect{.read = true});
  payload.exports.functions["make"].summary.assign(summary);
  std::string error;
  const std::optional<Payload> read = throughRecord(payload, error);
  ASSERT_TRUE(read) << error;
  EXPECT_EQ(read->exports.globals.find("unused"), unused);
  EXPECT_EQ(read->exports.globals.find("z_hook"), zHook);
  EXPECT_EQ(read->exports.functions.at("make").summary.get(), summary);
}

TEST(RecordPayload, KindsCarryTheirSource) {
  const core::PointerKind kind = core::PointerKind::counted(
      core::ExtentTerm::of(core::ExtentPath::ofParam(1)),
      core::Nullability::Nonnull, core::KindSource::Declared);
  const std::string spelled = spellKind(kind);
  EXPECT_EQ(spelled.rfind("declared counted(", 0), 0U) << spelled;
  const std::optional<core::PointerKind> parsed = parseKind(spelled);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, kind);
  EXPECT_EQ(parsed->source, core::KindSource::Declared);
  EXPECT_FALSE(parseKind("counted(param 1) nonnull"));
  EXPECT_FALSE(parseKind("sometimes single nullable"));
}

TEST(RecordPayload, SiteRowsAreTheLedgerWithoutEvidence) {
  core::UnitLedger unit;
  core::FunctionLedger function{.name = "f",
                                .file = "src/a.h",
                                .line = 3,
                                .linkage = core::Linkage::Internal,
                                .overBudget = false,
                                .requireSafe = false,
                                .callsSetjmp = false,
                                .sites = {}};
  core::Site site;
  site.ordinal = 0;
  site.kind = core::SiteKind::Call;
  site.location = at("src/a.h", 4, 9);
  site.text = "g(p)";
  site.callee = "g";
  site.boundary = core::Boundary::Call;
  site.addFacet(core::Facet::Temporal)
      .decide(core::FacetDecision::unresolvedFor(
          core::UnresolvedReason::UnknownCallee, "'g' may have freed 'p'"));
  function.sites.push_back(site);
  unit.functions.push_back(function);
  const std::vector<FunctionRows> rows = siteRows(unit);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].file, "src/a.h");
  EXPECT_EQ(rows[0].linkage, core::Linkage::Internal);
  ASSERT_EQ(rows[0].rows.size(), 1U);
  const auto &temporal = rows[0].rows[0].facets.at(
      static_cast<std::size_t>(core::Facet::Temporal));
  ASSERT_TRUE(temporal);
  // No detail: records carry no explanation text.
  EXPECT_TRUE(temporal->detail.empty());
  EXPECT_EQ(temporal->compact(), "unresolved/unknown-callee");
  const core::UnitLedger back = unitLedgerOf(rows);
  ASSERT_EQ(back.functions.size(), 1U);
  const core::Site &rebuilt = back.functions[0].sites.at(0);
  EXPECT_EQ(rebuilt.kind, core::SiteKind::Call);
  EXPECT_EQ(rebuilt.location.line, 4U);
  EXPECT_EQ(rebuilt.location.column, 9U);
  ASSERT_TRUE(rebuilt.facet(core::Facet::Temporal));
  EXPECT_TRUE(rebuilt.facet(core::Facet::Temporal)->decided);
  EXPECT_FALSE(rebuilt.facet(core::Facet::Spatial));
}

TEST(RecordPayload, WhatCannotBeReadIsNamed) {
  const llvm::json::Object good = toJson(fullPayload());
  const auto reason = [&](auto edit) {
    llvm::json::Object json = good;
    edit(json);
    std::string error;
    EXPECT_FALSE(payloadFromJson(json, "src/node.c", error));
    return error;
  };
  const auto function = [](llvm::json::Object &json) -> llvm::json::Object & {
    return *(*json.getArray("functions"))[0].getAsObject();
  };
  EXPECT_EQ(reason([](llvm::json::Object &json) { json.erase("a5"); }),
            "payload: missing key 'a5'");
  EXPECT_EQ(reason([&](llvm::json::Object &json) {
              function(json)["linkage"] = "weak";
            }),
            "payload.functions[0].linkage: unknown linkage 'weak'");
  EXPECT_EQ(reason([&](llvm::json::Object &json) {
              function(json)["summary"] = "summary\n  effect\nend\n";
            }).rfind("payload.functions[0].summary: ", 0),
            0U);
  EXPECT_EQ(reason([&](llvm::json::Object &json) {
              (*function(json).getObject("kinds"))["result"] = "single";
            }),
            "payload.functions[0].kinds.result: unknown kind 'single'");
  EXPECT_EQ(reason([&](llvm::json::Object &json) {
              (*json.getArray("functions"))
                  .push_back((*json.getArray("functions"))[0]);
            }),
            "payload.functions[1]: empty or duplicate function 'node_free'");
  EXPECT_EQ(reason([](llvm::json::Object &json) {
              json["definesAllocator"] = false;
            }),
            "payload.definesAllocator: disagrees with 'payload.a5.allocator'");
  const auto firstRow = [](llvm::json::Object &json) -> llvm::json::Array & {
    llvm::json::Object &rows = *(*json.getArray("sites"))[0].getAsObject();
    return *(*rows.getArray("rows"))[0].getAsArray();
  };
  EXPECT_EQ(reason([&](llvm::json::Object &json) { firstRow(json)[0] = 1; }),
            "payload.sites[0].rows[0][0]: ordinal 1, expected 0");
  EXPECT_EQ(reason([&](llvm::json::Object &json) {
              firstRow(json)[4] = "unresolved";
            }),
            "payload.sites[0].rows[0][4]: malformed facet 'unresolved'");
  EXPECT_EQ(reason([](llvm::json::Object &json) {
              auto &entry =
                  *(*json.getObject("interfaces")->getArray("globals"))[0]
                       .getAsObject();
              entry["type"] = "zz";
            }),
            "payload.interfaces.globals[0].type: invalid interface encoding");
  EXPECT_EQ(reason([](llvm::json::Object &json) {
              auto &entry =
                  *(*json.getObject("sizedFields")->getArray("witnesses"))[0]
                       .getAsObject();
              entry["productType"] = "i64";
            }),
            "payload.sizedFields.witnesses[0].productType: malformed "
            "product type 'i64'");
  EXPECT_EQ(reason([](llvm::json::Object &json) {
              auto &entry = *(*json.getArray("imports"))[0].getAsObject();
              auto &param =
                  *(*entry.getObject("declared")->getArray("params"))[0]
                       .getAsObject();
              param["ownership"] = "WEAVEC_SHARED";
            }),
            "payload.imports[0].declared.params[0].ownership: unknown "
            "annotation 'WEAVEC_SHARED'");
  EXPECT_EQ(reason([](llvm::json::Object &json) {
              auto &entry = *(*json.getArray("boundaries"))[0].getAsObject();
              entry["reason"] = "unknown-callee";
            }),
            "payload.boundaries[0].reason: not a boundary reason: "
            "'unknown-callee'");
}

TEST(RecordPayload, ContextRequestsAreBounded) {
  Payload payload;
  for (unsigned i = 0; i <= MaxContextRequests; ++i) {
    core::CallContext input;
    input.facts[SummaryPath::param(0)] = core::ValueFact::ofConstant(i);
    payload.exports.memoryRequests["invoke"].insert(input);
  }
  std::string error;
  EXPECT_FALSE(payloadFromJson(toJson(payload), "a.c", error));
  EXPECT_EQ(error.substr(error.find(": ") + 2), "too many memory requests");
}

} // namespace weavec::frontend::record
