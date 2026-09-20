//===- LedgerWriterTest.cpp - Tests for the RFC 0030 ledger writers -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/LedgerWriter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace weavec::frontend {

using core::Facet;
using core::FacetDecision;

TEST(LedgerWriter, NormalisesFingerprintMessages) {
  EXPECT_EQ(normalizeFingerprintMessage("use of 'p' after line 42"),
            "use of 'p' after line 0");
  EXPECT_EQ(normalizeFingerprintMessage("  a\t\tb  123x45 \n"), "a b 0x0");
  EXPECT_EQ(normalizeFingerprintMessage("b[1000]"), "b[0]");
  EXPECT_EQ(normalizeFingerprintMessage(""), "");
  EXPECT_EQ(normalizeFingerprintMessage(" \t "), "");
}

TEST(LedgerWriter, FingerprintsFollowTheFormula) {
  // hex(SHA-256("weavec-fp/1" US key US path US function US message US
  // ordinal))[0:32].
  const std::string input = std::string("weavec-fp/1") + '\x1f' +
                            "use-after-free" + '\x1f' + "src/a.c" + '\x1f' +
                            "f" + '\x1f' + "use of 'p'" + '\x1f' + "2";
  const std::string expected =
      llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(input)),
                  /*LowerCase=*/true)
          .substr(0, 32);
  const std::string actual =
      computeFingerprint("use-after-free", "src/a.c", "f", "use of 'p'", 2);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(actual.size(), 32U);
  EXPECT_NE(actual, computeFingerprint("use-after-free", "src/a.c", "f",
                                       "use of 'p'", 1));
}

TEST(LedgerWriter, RelativisesPaths) {
  EXPECT_EQ(pathRelativeToRoot("/proj/src/a.c", "/proj", "/"), "src/a.c");
  EXPECT_EQ(pathRelativeToRoot("/proj/src/a.c", "/proj/", "/"), "src/a.c");
  EXPECT_EQ(pathRelativeToRoot("/proj", "/proj", "/"), ".");
  EXPECT_EQ(pathRelativeToRoot("/projects/a.c", "/proj", "/"), "/projects/a.c");
  EXPECT_EQ(pathRelativeToRoot("/other/c.c", "/proj", "/"), "/other/c.c");
  EXPECT_EQ(pathRelativeToRoot("../lib/b.c", "/proj", "/proj/src"), "lib/b.c");
  EXPECT_EQ(pathRelativeToRoot("./src/./a.c", "/proj", "/proj"), "src/a.c");
  EXPECT_EQ(pathRelativeToRoot("a/b.c", "/", "/"), "a/b.c");
  EXPECT_EQ(pathRelativeToRoot("", "/proj", "/"), "");
}

/// The §12.1 example: one deref site of `cJSON_Delete`.
static core::Ledger cjsonLedger() {
  core::Ledger ledger;
  ledger.producer = core::Producer{
      .name = "weavec", .version = "0.11.0", .revision = "abc1234"};
  ledger.root = "/proj";
  core::UnitLedger unit{.source = "/proj/src/cJSON.c",
                        .object = "/proj/build/cJSON.o",
                        .target = "arm64-apple-macosx15.0"};
  core::FunctionLedger function{
      .name = "cJSON_Delete", .line = 253, .linkage = core::Linkage::External};
  core::Site site{.ordinal = 0,
                  .kind = core::SiteKind::Deref,
                  .location = {.file = "/proj/src/cJSON.c",
                               .line = 258,
                               .column = 21,
                               .opaque = 0},
                  .text = core::siteText("item -> next")};
  core::FacetRecord &null = site.addFacet(Facet::Null);
  null.decide(FacetDecision::checked());
  null.check = core::FacetCheck{.kind = core::CheckTemplate::Nonnull};
  site.addFacet(Facet::Spatial).decide(FacetDecision::proven());
  site.addFacet(Facet::Temporal)
      .decide(FacetDecision::unresolvedFor(
          core::UnresolvedReason::UnknownCallee,
          "'global_hooks.deallocate' may have released 'item'"));
  function.sites.push_back(site);
  unit.functions.push_back(function);
  ledger.units.push_back(unit);
  return ledger;
}

static std::string cjsonFingerprint(llvm::StringRef key) {
  return computeFingerprint(key, "src/cJSON.c", "cJSON_Delete", "item->next",
                            0);
}

TEST(LedgerWriter, GoldenUnitLedger) {
  const std::string counts =
      R"("sites":1,"proven":0,"checked":0,"violation":0,"unresolved":1,"trusted":0,)"
      R"("facets":{"spatial":{"proven":1,"checked":0,"violation":0,"unresolved":0,"trusted":0},)"
      R"("null":{"proven":0,"checked":1,"violation":0,"unresolved":0,"trusted":0},)"
      R"("temporal":{"proven":0,"checked":0,"violation":0,"unresolved":1,"trusted":0},)"
      R"("assertion":{"proven":0,"checked":0,"violation":0,"unresolved":0,"trusted":0}},)"
      R"("unresolvedReasons":{"unknown-extent":0,"unknown-index":0,"inexpressible":0,)"
      R"("may-released":0,"may-moved":0,"may-alias-released":0,"may-invalid-release":0,)"
      R"("may-mismatched-release":0,"may-dangle":0,"may-conflict":0,"unknown-callee":1,)"
      R"("callback":0,"setjmp":0,"budget":0,"unanalysed":0,"raw-cast":0,)"
      R"("dangling-escape":0,"second-owner":0,"no-zero-init":0},)"
      R"("trustedReasons":{"unsafe":0,"system-api":0,"library-spec":0,)"
      R"("extern-contract":0,"caller-contract":0,"external-unit":0,"concurrency":0},)"
      R"("unresolvedShare":{"spatialNull":0},"errors":0,"warnings":0,)"
      R"("functions":1,"overBudget":[])";
  const std::string expected =
      R"({"schema":"weavec-ledger","version":1,)"
      R"("producer":{"name":"weavec","version":"0.11.0","revision":"abc1234"},)"
      R"("scope":"unit","root":"/proj",)"
      R"("config":{"checks":"trap","zeroInit":true,"require":"none","budget":50000},)"
      R"("summary":{)" +
      counts +
      R"(},"units":[{"source":"src/cJSON.c","object":"build/cJSON.o",)"
      R"("target":"arm64-apple-macosx15.0","summary":{)" +
      counts +
      R"(,"a5":{"nonLoweredAllocations":0,"bypassedDeclarations":0}},)"
      R"("functions":[{"name":"cJSON_Delete","file":"src/cJSON.c","line":253,)"
      R"("linkage":"external",)"
      R"("overBudget":false,"requireSafe":false,"setjmp":false,)"
      R"("sites":[{"ordinal":0,"kind":"deref","line":258,"column":21,)"
      R"("text":"item->next","boundary":null,"callee":null,"facets":{)"
      R"("null":{"outcome":"checked","check":{"template":"nonnull"},"fingerprint":")" +
      cjsonFingerprint("deref/null/checked") +
      R"("},"spatial":{"outcome":"proven","fingerprint":")" +
      cjsonFingerprint("deref/spatial/proven") +
      R"("},"temporal":{"outcome":"unresolved","reason":"unknown-callee",)"
      R"("detail":"'global_hooks.deallocate' may have released 'item'",)"
      R"("fixit":null,"requirements":[],"diagnostic":null,"fingerprint":")" +
      cjsonFingerprint("deref/temporal/unresolved/unknown-callee") +
      R"("}}}]}]}],"diagnostics":[]})"
      "\n";
  EXPECT_EQ(renderLedgerJson(cjsonLedger(), {.indent = 0}), expected);
}

TEST(LedgerWriter, SiteTextIsNormalisedOnOutput) {
  // A producer that stored raw source text still gets the §12.1 form, and
  // the same fingerprint.
  core::Ledger raw = cjsonLedger();
  raw.units[0].functions[0].sites[0].text = "item\n  ->\tnext";
  EXPECT_EQ(renderLedgerJson(raw, {.indent = 0}),
            renderLedgerJson(cjsonLedger(), {.indent = 0}));
  raw.units[0].functions[0].sites[0].text = std::string(100, 'x');
  EXPECT_NE(renderLedgerJson(raw, {.indent = 0})
                .find("\"text\":\"" + std::string(80, 'x') + "\","),
            std::string::npos);
}

TEST(LedgerWriter, PrettyOutputIsIndented) {
  const std::string text = renderLedgerJson(cjsonLedger());
  EXPECT_EQ(
      text.rfind("{\n  \"schema\": \"weavec-ledger\",\n  \"version\": 1,", 0),
      0U);
  EXPECT_TRUE(llvm::StringRef(text).ends_with("\n}\n"));
  EXPECT_EQ(renderLedger(cjsonLedger(), LedgerFormat::Json), text);
}

TEST(LedgerWriter, AssignedFingerprintsAreUsed) {
  core::Ledger ledger = cjsonLedger();
  assignFingerprints(ledger);
  const core::FacetRecord *temporal =
      ledger.units[0].functions[0].sites[0].facet(Facet::Temporal);
  EXPECT_EQ(temporal->fingerprint,
            cjsonFingerprint("deref/temporal/unresolved/unknown-callee"));
  ledger.units[0].functions[0].sites[0].facet(Facet::Spatial)->fingerprint =
      "preset";
  const std::string text = renderLedgerJson(ledger, {.indent = 0});
  EXPECT_NE(text.find(R"("outcome":"proven","fingerprint":"preset")"),
            std::string::npos);
}

TEST(LedgerWriter, OrdinalsCountEqualRowsInLineOrder) {
  // Two accesses with the same key and normalised text: the earlier line
  // gets ordinal 0 whatever the site order, and the fingerprints survive
  // renumbered lines and changed constants.
  core::Ledger ledger = cjsonLedger();
  core::FunctionLedger &function = ledger.units[0].functions[0];
  function.sites.clear();
  for (const auto &[line, text] :
       std::vector<std::pair<std::uint32_t, std::string>>{{10, "a[2]"},
                                                          {5, "a[1]"}}) {
    core::Site site{.ordinal =
                        static_cast<std::uint32_t>(function.sites.size()),
                    .kind = core::SiteKind::Index,
                    .location = {.file = "/proj/src/cJSON.c",
                                 .line = line,
                                 .column = 3,
                                 .opaque = 0},
                    .text = text};
    site.addFacet(Facet::Spatial).decide(FacetDecision::checked());
    function.sites.push_back(site);
  }
  assignFingerprints(ledger);
  const auto print = [&](std::size_t site) {
    return function.sites[site].facet(Facet::Spatial)->fingerprint;
  };
  const auto expected = [](std::size_t ordinal) {
    return computeFingerprint("index/spatial/checked", "src/cJSON.c",
                              "cJSON_Delete", "a[0]", ordinal);
  };
  EXPECT_EQ(print(1), expected(0));
  EXPECT_EQ(print(0), expected(1));
  core::Ledger moved = ledger;
  for (core::Site &site : moved.units[0].functions[0].sites) {
    site.location.line += 100;
    site.text = site.text == "a[2]" ? "a[7]" : "a[3]";
    site.facet(Facet::Spatial)->fingerprint.clear();
  }
  assignFingerprints(moved);
  EXPECT_EQ(
      moved.units[0].functions[0].sites[1].facet(Facet::Spatial)->fingerprint,
      expected(0));
}

TEST(LedgerWriter, HeaderFunctionsCarryTheirFile) {
  // §12.1 (S3 amendment): a function defined in a header names its file,
  // and its sites, which carry no file of their own, take it, in the JSON,
  // the fingerprints and SARIF.
  core::Ledger ledger = cjsonLedger();
  core::FunctionLedger &function = ledger.units[0].functions[0];
  function.file = "/proj/include/buffer.h";
  function.sites[0].location.file.clear();
  const std::string json = renderLedgerJson(ledger, {.indent = 0});
  EXPECT_NE(json.find(R"("name":"cJSON_Delete","file":"include/buffer.h",)"),
            std::string::npos);
  assignFingerprints(ledger);
  EXPECT_EQ(function.sites[0].facet(Facet::Spatial)->fingerprint,
            computeFingerprint("deref/spatial/proven", "include/buffer.h",
                               "cJSON_Delete", "item->next", 0));
  const std::string sarif = renderLedgerSarif(ledger, {.indent = 0});
  EXPECT_NE(sarif.find(R"("uri":"include/buffer.h","uriBaseId":"SRCROOT")"),
            std::string::npos);
}

/// A program ledger with requirements, a violation, diagnostics, a path
/// outside the root and a verify check.
static core::Ledger programLedger() {
  core::Ledger ledger = cjsonLedger();
  ledger.scope = core::LedgerScope::Program;
  ledger.config.checks = core::ChecksMode::Verify;
  ledger.assumptions = core::Assumptions{};
  ledger.assumptions->a1 = {.exportedRequirements = 14,
                            .verified = 11,
                            .reliesOnSingle = 40,
                            .unverifiedCallers = 3};
  ledger.assumptions->a3 = {.headerInvariants = 2,
                            .unverified = 0,
                            .inputsWithoutRecords = {"liblua.a"}};
  ledger.assumptions->a5.nonLoweredAllocations = 4;
  core::UnitLedger other{
      .source = "/elsewhere/b.c", .object = "", .target = "t"};
  core::FunctionLedger f{
      .name = "f", .line = 1, .linkage = core::Linkage::Internal};
  core::Site copy{.ordinal = 0,
                  .kind = core::SiteKind::LibCall,
                  .text = "memcpy(d,s,n)",
                  .callee = "memcpy"};
  core::FacetRecord &spatial = copy.addFacet(Facet::Spatial);
  spatial.addRequirement(
      {.argument = 0,
       .need = "n",
       .have = "16",
       .decision = FacetDecision::checked(),
       .check = core::FacetCheck{.kind = core::CheckTemplate::Len}});
  spatial.addRequirement({.argument = 1,
                          .need = "n",
                          .decision = FacetDecision::unresolvedFor(
                              core::UnresolvedReason::UnknownExtent)});
  core::Site deref{.ordinal = 1, .kind = core::SiteKind::Deref, .text = "*p"};
  core::FacetRecord &null = deref.addFacet(Facet::Null);
  null.decide(FacetDecision::violation());
  null.diagnostic = 0;
  null.fixit = core::FixItHint{.location = {.file = "/elsewhere/b.c",
                                            .line = 1,
                                            .column = 8,
                                            .opaque = 0},
                               .insertion = "WEAVEC_NONNULL "};
  core::FacetRecord &proven = deref.addFacet(Facet::Spatial);
  proven.decide(FacetDecision::proven());
  proven.check =
      core::FacetCheck{.kind = core::CheckTemplate::Index, .proven = true};
  deref.addFacet(Facet::Temporal)
      .decide(FacetDecision::trustedFor(core::TrustReason::Unsafe));
  f.sites = {copy, deref};
  other.functions.push_back(f);
  ledger.units.push_back(other);
  ledger.diagnostics.push_back(core::LedgerDiagnostic{
      .id = "use-after-free",
      .severity = core::Severity::Warning,
      .certainty = core::Certainty::Possible,
      .message = "use of 'p' after it may have been freed",
      .location =
          {.file = "/proj/src/a.c", .line = 7, .column = 3, .opaque = 0},
      .function = "f",
      .site = 1,
      .facet = Facet::Temporal,
      .notes = {{.message = "freed here on some paths",
                 .location = {.file = "/proj/src/a.c",
                              .line = 5,
                              .column = 3,
                              .opaque = 0}}}});
  ledger.diagnostics.push_back(core::LedgerDiagnostic{
      .id = "invalid-annotation",
      .severity = core::Severity::Warning,
      .message = "'n' in WEAVEC_COUNTED_BY does not name a parameter or field",
      .location = {.file = "src/a.c", .line = 2, .column = 1, .opaque = 0}});
  return ledger;
}

TEST(LedgerWriter, ProgramLedgerSnippets) {
  const std::string text = renderLedgerJson(
      programLedger(), {.workingDirectory = "/proj", .indent = 0});
  const auto expectSnippet = [&](const std::string &snippet) {
    EXPECT_NE(text.find(snippet), std::string::npos) << snippet;
  };
  expectSnippet(R"("scope":"program")");
  expectSnippet(R"("config":{"checks":"verify","zeroInit":true,)"
                R"("require":"none","budget":50000})");
  expectSnippet(
      R"("assumptions":{"A1":{"exportedRequirements":14,"verified":11,)"
      R"("reliesOnSingle":40,"unverifiedCallers":3},"A3":{)"
      R"("headerInvariants":2,"unverified":0,"inputsWithoutRecords":)"
      R"(["liblua.a"]},"A4":{"concurrencySites":0},"A5":{)"
      R"("nonLoweredAllocations":4,"bypassedDeclarations":0,)"
      R"("allocatorDefinedBy":null}},"units":[)");
  expectSnippet(R"("source":"/elsewhere/b.c","object":null,"target":"t")");
  expectSnippet(
      R"("spatial":{"outcome":"unresolved","reason":"unknown-extent",)"
      R"("detail":null,"fixit":null,"requirements":[{"arg":0,"need":"n",)"
      R"("have":"16","outcome":"checked","check":{"template":"len"}},)"
      R"({"arg":1,"need":"n","have":null,"outcome":"unresolved",)"
      R"("reason":"unknown-extent"}],"diagnostic":null,"fingerprint":")");
  expectSnippet(
      R"("null":{"outcome":"violation","reason":null,"detail":null,)"
      R"("fixit":{"file":"/elsewhere/b.c","line":1,"column":8,)"
      R"("insertion":"WEAVEC_NONNULL "},"requirements":[],"diagnostic":0,)"
      R"("fingerprint":")");
  expectSnippet(R"("spatial":{"outcome":"proven","check":{"template":"index",)"
                R"("proven":true},)");
  expectSnippet(
      R"("temporal":{"outcome":"trusted","reason":"unsafe","detail":null,)");
  // Gate G6: the verify coverage of the proven spatial and null facets.
  expectSnippet(R"("functions":2,"overBudget":[],"verifyChecks":1,)"
                R"("verifyCoverage":{"spatial":{"proven":2,"checked":1},)"
                R"("null":{"proven":0,"checked":0}}})");
  expectSnippet(
      R"({"id":"use-after-free","severity":"warning","certainty":"possible",)"
      R"("message":"use of 'p' after it may have been freed",)"
      R"("file":"src/a.c","line":7,"column":3,"function":"f","site":1,)"
      R"("facet":"temporal","notes":[{"message":"freed here on some paths",)"
      R"("file":"src/a.c","line":5,"column":3}],"fingerprint":")");
  expectSnippet(
      R"("file":"src/a.c","line":2,"column":1,"function":null,"site":null,)"
      R"("facet":null,"notes":[],"fingerprint":")");
  // One unresolved facet of five spatial and null facets.
  expectSnippet(R"("unresolvedShare":{"spatialNull":0.2})");
  core::Ledger ledger = programLedger();
  assignFingerprints(ledger, {.workingDirectory = "/proj"});
  EXPECT_EQ(ledger.diagnostics[1].fingerprint,
            computeFingerprint(
                "invalid-annotation", "src/a.c", FileScopeFunction,
                normalizeFingerprintMessage(ledger.diagnostics[1].message), 0));
}

static const llvm::json::Object &objectAt(const llvm::json::Value &value) {
  const llvm::json::Object *object = value.getAsObject();
  EXPECT_NE(object, nullptr);
  static const llvm::json::Object Empty;
  return object != nullptr ? *object : Empty;
}

TEST(LedgerWriter, SarifStructure) {
  core::Ledger ledger = programLedger();
  ledger.units[0].functions[0].sites[0].location.file = "/proj/src/my file.c";
  assignFingerprints(ledger, {.workingDirectory = "/proj"});
  const std::string text = renderLedgerSarif(
      ledger, {.workingDirectory = "/proj", .executionSuccessful = false});
  llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(text);
  ASSERT_TRUE(static_cast<bool>(parsed)) << llvm::toString(parsed.takeError());
  const llvm::json::Object &root = objectAt(*parsed);
  EXPECT_EQ(root.getString("version"), "2.1.0");
  const llvm::json::Array *runs = root.getArray("runs");
  ASSERT_TRUE(runs && runs->size() == 1);
  const llvm::json::Object &run = objectAt((*runs)[0]);
  const llvm::json::Object &driver =
      *run.getObject("tool")->getObject("driver");
  EXPECT_EQ(driver.getString("name"), "weavec");
  EXPECT_EQ(driver.getString("semanticVersion"), "0.11.0");
  const llvm::json::Array &rules = *driver.getArray("rules");
  // Two diagnostic ids, 19 unresolved and 7 trusted reasons.
  ASSERT_EQ(rules.size(), 2U + 19U + 7U);
  EXPECT_EQ(objectAt(rules[0]).getString("id"), "invalid-annotation");
  EXPECT_EQ(objectAt(rules[1]).getString("id"), "use-after-free");
  EXPECT_EQ(objectAt(rules[2]).getString("id"), "unresolved/unknown-extent");
  EXPECT_EQ(objectAt(rules[21]).getString("id"), "trusted/unsafe");
  EXPECT_EQ(run.getObject("originalUriBaseIds")
                ->getObject("SRCROOT")
                ->getString("uri"),
            "file:///proj/");
  EXPECT_EQ(objectAt((*run.getArray("invocations"))[0])
                .getBoolean("executionSuccessful"),
            false);
  const llvm::json::Array &results = *run.getArray("results");
  // Two diagnostics, two unresolved facets and one trusted facet; proven,
  // checked and violation facets are not results.
  ASSERT_EQ(results.size(), 5U);
  const llvm::json::Object &uaf = objectAt(results[0]);
  EXPECT_EQ(uaf.getString("ruleId"), "use-after-free");
  EXPECT_EQ(uaf.getInteger("ruleIndex"), 1);
  EXPECT_EQ(uaf.getString("level"), "warning");
  EXPECT_EQ(uaf.getString("kind"), "fail");
  const llvm::json::Object &location =
      *objectAt((*uaf.getArray("locations"))[0]).getObject("physicalLocation");
  EXPECT_EQ(location.getObject("artifactLocation")->getString("uri"),
            "src/a.c");
  EXPECT_EQ(location.getObject("artifactLocation")->getString("uriBaseId"),
            "SRCROOT");
  EXPECT_EQ(location.getObject("region")->getInteger("startLine"), 7);
  EXPECT_EQ(location.getObject("region")->getInteger("startColumn"), 3);
  EXPECT_EQ(uaf.getArray("relatedLocations")->size(), 1U);
  EXPECT_EQ(uaf.getObject("partialFingerprints")->getString("weavec/v1"),
            ledger.diagnostics[0].fingerprint);
  EXPECT_EQ(uaf.getObject("properties")->getString("certainty"), "possible");
  const llvm::json::Object &open = objectAt(results[2]);
  EXPECT_EQ(open.getString("ruleId"), "unresolved/unknown-callee");
  EXPECT_EQ(open.getString("level"), "note");
  EXPECT_EQ(open.getString("kind"), "open");
  EXPECT_EQ(open.getObject("message")->getString("text"),
            "temporal of deref 'item->next' is unresolved: "
            "'global_hooks.deallocate' may have released 'item'");
  EXPECT_EQ(objectAt((*open.getArray("locations"))[0])
                .getObject("physicalLocation")
                ->getObject("artifactLocation")
                ->getString("uri"),
            "src/my%20file.c");
  const llvm::json::Object &review = objectAt(results[4]);
  EXPECT_EQ(review.getString("ruleId"), "trusted/unsafe");
  EXPECT_EQ(review.getString("level"), "none");
  EXPECT_EQ(review.getString("kind"), "review");
  EXPECT_EQ(objectAt((*review.getArray("locations"))[0])
                .getObject("physicalLocation")
                ->getObject("artifactLocation")
                ->getString("uri"),
            "file:///elsewhere/b.c");
  for (const llvm::json::Value &result : results) {
    const llvm::json::Object &object = objectAt(result);
    const auto index = object.getInteger("ruleIndex");
    ASSERT_TRUE(index);
    EXPECT_EQ(objectAt(rules[static_cast<std::size_t>(*index)]).getString("id"),
              object.getString("ruleId"));
  }
  EXPECT_EQ(run.getObject("properties")
                ->getObject("weavec")
                ->getObject("summary")
                ->getInteger("sites"),
            3);
}

/// A fresh directory under the system temporary directory.
static std::string temporaryDirectory() {
  llvm::SmallString<256> path;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("weavec-ledger", path));
  return path.str().str();
}

TEST(LedgerWriter, DetectsTheFingerprintRoot) {
  const std::string base = temporaryDirectory();
  const std::string project = base + "/proj";
  ASSERT_FALSE(llvm::sys::fs::create_directories(project + "/.git"));
  ASSERT_FALSE(llvm::sys::fs::create_directories(project + "/src/deep"));
  EXPECT_EQ(detectFingerprintRoot("src/deep/a.c", project), project);
  EXPECT_EQ(detectFingerprintRoot(project + "/src/../src/a.c", "/"), project);
  // A worktree's `.git` is a file.
  const std::string worktree = base + "/tree";
  ASSERT_FALSE(llvm::sys::fs::create_directories(worktree + "/src"));
  {
    std::error_code ec;
    llvm::raw_fd_ostream marker(worktree + "/.git", ec);
    ASSERT_FALSE(ec);
    marker << "gitdir: elsewhere\n";
  }
  EXPECT_EQ(detectFingerprintRoot(worktree + "/src/b.c", "/"), worktree);
  // Without `.git` the root is the working directory (assuming none above
  // the system temporary directory).
  const std::string loose = base + "/loose";
  ASSERT_FALSE(llvm::sys::fs::create_directories(loose));
  EXPECT_EQ(detectFingerprintRoot(loose + "/c.c", loose + "/./"), loose);
  EXPECT_EQ(detectFingerprintRoot("", loose), loose);
  std::ignore = llvm::sys::fs::remove_directories(base);
}

TEST(LedgerWriter, LedgerPathsFollowTheDirectoryForm) {
  const std::string base = temporaryDirectory();
  EXPECT_TRUE(isLedgerDirectory("ledgers/"));
  EXPECT_TRUE(isLedgerDirectory(base));
  EXPECT_FALSE(isLedgerDirectory(base + "/missing"));
  EXPECT_FALSE(isLedgerDirectory(""));
  EXPECT_EQ(ledgerPathFor("ledgers/", "build/cJSON.o"),
            "ledgers/cJSON.o.ledger.json");
  EXPECT_EQ(ledgerPathFor(base, "minigzip"), base + "/minigzip.ledger.json");
  EXPECT_EQ(ledgerPathFor("out.json", "build/cJSON.o"), "out.json");
  std::ignore = llvm::sys::fs::remove_directories(base);
}

TEST(LedgerWriter, WritesAtomically) {
  const std::string base = temporaryDirectory();
  const std::string path = base + "/a.ledger.json";
  std::string error;
  ASSERT_TRUE(writeLedger(path, cjsonLedger(), LedgerFormat::Sarif, {}, &error))
      << error;
  const auto buffer = llvm::MemoryBuffer::getFile(path);
  ASSERT_TRUE(buffer);
  EXPECT_EQ((*buffer)->getBuffer(), renderLedgerSarif(cjsonLedger()));
  // Only the ledger is left behind.
  std::error_code ec;
  std::size_t entries = 0;
  for (llvm::sys::fs::directory_iterator it(base, ec), end; it != end && !ec;
       it.increment(ec))
    ++entries;
  EXPECT_EQ(entries, 1U);
  EXPECT_FALSE(writeFileAtomically(base + "/missing/x.json", "{}", &error));
  EXPECT_NE(error.find("missing/x.json"), std::string::npos) << error;
  std::ignore = llvm::sys::fs::remove_directories(base);
}

TEST(LedgerWriter, FormatSpellings) {
  EXPECT_EQ(parseLedgerFormat("sarif"), LedgerFormat::Sarif);
  EXPECT_EQ(parseLedgerFormat("json"), LedgerFormat::Json);
  EXPECT_FALSE(parseLedgerFormat("xml"));
  EXPECT_EQ(toString(LedgerFormat::Sarif), "sarif");
}

} // namespace weavec::frontend
