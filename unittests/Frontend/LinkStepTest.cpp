//===- LinkStepTest.cpp - Tests for the link step (RFC 0030 §13.2) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/LinkStep.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace weavec::frontend {
namespace {

using core::FacetDecision;
using core::SlotKey;

core::SourceLocation at(std::string file, std::uint32_t line,
                        std::uint32_t column) {
  return core::SourceLocation{
      .file = std::move(file), .line = line, .column = column, .opaque = 0};
}

ProgramMember member(std::string source) {
  ProgramMember unit;
  unit.source = std::move(source);
  unit.cwd = "/work";
  unit.object = unit.source + ".o";
  unit.target = "arm64-apple-macosx15.0";
  unit.payload.exports.source = unit.source;
  return unit;
}

/// `name` defined in `unit` with external linkage and `summary`.
void define(ProgramMember &unit, const std::string &name,
            core::FunctionSummary summary,
            record::FunctionInterface facts = {}) {
  unit.payload.exports.functions[name].summary.assign(std::move(summary));
  unit.payload.facts.functions[name] = std::move(facts);
}

core::FunctionSummary freesParam(std::uint32_t param) {
  core::FunctionSummary summary;
  summary.addEffect(core::SummaryPath::param(param),
                    core::PlaceEffect{.freed = true});
  return summary;
}

/// A function of a run ledger with one site per entry of `sites`: its kind,
/// callee and temporal decision.
struct SiteSpec {
  core::SiteKind kind;
  std::string callee;
  std::optional<FacetDecision> temporal;
};
core::FunctionLedger ledgerFunction(const std::string &name,
                                    std::vector<SiteSpec> sites,
                                    core::Linkage linkage) {
  core::FunctionLedger function{.name = name,
                                .file = "",
                                .line = 1,
                                .linkage = linkage,
                                .overBudget = false,
                                .requireSafe = false,
                                .callsSetjmp = false,
                                .sites = {}};
  for (std::uint32_t i = 0; i < sites.size(); ++i) {
    core::Site site;
    site.ordinal = i;
    site.kind = sites[i].kind;
    site.location = at("", 10 + i, 3);
    site.callee = sites[i].callee;
    if (sites[i].kind == core::SiteKind::Call)
      site.boundary = core::Boundary::Call;
    if (sites[i].temporal)
      site.addFacet(core::Facet::Temporal).decide(*sites[i].temporal);
    function.sites.push_back(std::move(site));
  }
  return function;
}

core::Ledger runOf(std::vector<core::FunctionLedger> functions) {
  core::Ledger ledger;
  ledger.units.emplace_back();
  ledger.units.front().functions = std::move(functions);
  return ledger;
}

const core::Site &siteOf(const core::Ledger &ledger, std::size_t unit,
                         const std::string &function, std::uint32_t ordinal) {
  for (const core::FunctionLedger &entry : ledger.units.at(unit).functions)
    if (entry.name == function)
      return entry.sites.at(ordinal);
  ADD_FAILURE() << "no function " << function;
  static const core::Site None;
  return None;
}

} // namespace

TEST(LinkStep, DisplayPathsAreRelativeToTheLinkWhenInside) {
  EXPECT_EQ(displayPath("src/a.c", "/work", "/work"), "src/a.c");
  EXPECT_EQ(displayPath("/work/src/../a.c", "/elsewhere", "/work"), "a.c");
  EXPECT_EQ(displayPath("a.c", "/other", "/work"), "/other/a.c");
  EXPECT_EQ(displayPath("", "/work", "/work"), "");
}

TEST(LinkStep, SlotsAreSolvedAcrossRecords) {
  // Lua's shape: `lstate.c` stores `new_state`'s parameter into a field and
  // calls through it; `lua.c` passes its own `l_alloc`.
  ProgramMember state = member("state.c");
  const SlotKey field = SlotKey::field("struct global_state", "frealloc");
  state.payload.facts.slots = record::SlotFacts{
      .rows = {core::SlotRow{.slot = field,
                             .targets = {},
                             .sources = {SlotKey::param("new_state", 0)},
                             .open = std::nullopt}},
      .unit = "state.c",
      .defined = {"new_state", "state_release"},
      .exported = {"new_state", "state_release"},
      .confinedRecords = {},
      .escapedStatics = {}};
  ProgramMember main = member("main.c");
  main.payload.facts.slots = record::SlotFacts{
      .rows = {core::SlotRow{.slot = SlotKey::param("new_state", 0),
                             .targets = {"main.c:l_alloc"},
                             .sources = {},
                             .open = std::nullopt}},
      .unit = "main.c",
      .defined = {"main", "main.c:l_alloc"},
      .exported = {"main"},
      .confinedRecords = {},
      .escapedStatics = {}};
  const std::vector<ProgramMember> members{state, main};
  const core::SlotSolution closed = solveProgramSlots(members, LinkShape{});
  EXPECT_EQ(closed.targets(field), std::set<std::string>{"main.c:l_alloc"});
  EXPECT_TRUE(closed.isClosed(field));
  EXPECT_EQ(closed.resolveCall(field).kind,
            core::IndirectCallKind::ClosedSingle);

  // Code without a record may store into the field too.
  const core::SlotSolution open = solveProgramSlots(
      members, LinkShape{.executable = true,
                         .exportDynamic = false,
                         .inputsWithoutRecords = {"liblua.a"}});
  EXPECT_TRUE(open.isOpen(field));
  EXPECT_EQ(open.resolveCall(field).kind, core::IndirectCallKind::OpenKnown);
  // So may code a shared library's users run.
  EXPECT_TRUE(solveProgramSlots(members, LinkShape{.executable = false,
                                                   .exportDynamic = false,
                                                   .inputsWithoutRecords = {}})
                  .isOpen(field));

  std::string text;
  llvm::raw_string_ostream os(text);
  dumpProgramSlots(closed, os);
  EXPECT_NE(text.find("field struct global_state frealloc: {main.c:l_alloc} "
                      "closed"),
            std::string::npos)
      << text;
}

TEST(LinkStep, BoundaryRowsCarryTheirUnit) {
  ProgramMember a = member("a.c");
  a.payload.facts.boundaries = {
      analysis::BoundaryRow{.unit = {},
                            .function = "remember",
                            .site = 2,
                            .reason = core::UnresolvedReason::DanglingEscape,
                            .placeClass = "g_cache"}};
  const std::vector<analysis::BoundaryRow> rows =
      programBoundaries(std::vector<ProgramMember>{a});
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].unit, "a.c");
  EXPECT_EQ(rows[0].placeClass, "g_cache");
}

TEST(LinkStep, ALyingDeclarationIsAnErrorAtTheDeclaration) {
  // Probe 38: the caller declares `inspect` WEAVEC_BORROWED; its definition
  // frees the parameter.
  ProgramMember caller = member("main.c");
  caller.payload.exports.imports = {"inspect"};
  caller.payload.facts.imports["inspect"] = record::ImportInterface{
      .declared =
          record::DeclaredInterface{
              .params = {record::DeclaredParam{.name = "p",
                                               .kind = std::nullopt,
                                               .ownership = "WEAVEC_BORROWED"}},
              .result = std::nullopt,
              .ownership = std::nullopt},
      .location = at("main.c", 6, 6),
      .calls = {}};
  ProgramMember callee = member("impl.c");
  define(callee, "inspect", freesParam(0),
         record::FunctionInterface{.params = {"default single nullable"},
                                   .result = std::nullopt,
                                   .reliesOnSingle = {},
                                   .requirements = {},
                                   .location = at("impl.c", 4, 6)});
  const std::vector<ProgramMember> members{caller, callee};
  const DeclarationCheck check = verifyDeclarations(members, "/work");
  ASSERT_EQ(check.diagnostics.size(), 1U);
  const core::Diagnostic &error = check.diagnostics.front();
  EXPECT_EQ(error.id, core::diag::AnnotationMismatch);
  EXPECT_EQ(error.severity, core::Severity::Error);
  EXPECT_EQ(error.certainty, core::Certainty::Definite);
  EXPECT_EQ(error.message, "'inspect' is declared WEAVEC_BORROWED here but "
                           "its definition in 'impl.c' frees 'p'");
  EXPECT_EQ(error.location, at("/work/main.c", 6, 6));
  ASSERT_EQ(error.notes.size(), 1U);
  EXPECT_EQ(error.notes.front().message, "defined here");
  EXPECT_EQ(error.notes.front().location, at("/work/impl.c", 4, 6));
  ASSERT_EQ(check.contradictions.size(), 1U);
  EXPECT_EQ(check.contradictions.front().member, 0U);
  EXPECT_EQ(check.contradictions.front().definer, 1U);
  EXPECT_EQ(check.contradictions.front().function, "inspect");

  // A definition that keeps its word is not reported.
  std::vector<ProgramMember> honest{caller, member("impl.c")};
  define(honest[1], "inspect", core::FunctionSummary{});
  EXPECT_TRUE(verifyDeclarations(honest, "/work").diagnostics.empty());
  // Nor is a declaration no other unit defines.
  EXPECT_TRUE(verifyDeclarations(std::vector<ProgramMember>{caller}, "/work")
                  .diagnostics.empty());
}

TEST(LinkStep, ResultContractsAreVerified) {
  ProgramMember caller = member("main.c");
  caller.payload.facts.imports["get"] = record::ImportInterface{
      .declared = record::DeclaredInterface{.params = {},
                                            .result = "unknown nonnull",
                                            .ownership = "WEAVEC_OWNED"},
      .location = at("main.c", 2, 7),
      .calls = {}};
  ProgramMember callee = member("get.c");
  core::FunctionSummary summary;
  summary.addReturn(core::ValueSource::borrow(core::SummaryPath::param(0)));
  summary.addReturn(core::ValueSource::null());
  define(callee, "get", summary);
  const DeclarationCheck check =
      verifyDeclarations(std::vector<ProgramMember>{caller, callee}, "/work");
  ASSERT_EQ(check.diagnostics.size(), 1U + 1U);
  EXPECT_EQ(check.diagnostics[0].message,
            "'get' is declared WEAVEC_OWNED here but its definition in "
            "'get.c' returns a borrowed pointer");
  EXPECT_EQ(check.diagnostics[1].message,
            "'get' is declared to return unknown nonnull here but its "
            "definition in 'get.c' may return null");
}

TEST(LinkStep, ViolatedHeaderInvariantsAreErrorsAtTheStore) {
  ProgramMember writer = member("a.c");
  writer.payload.facts.invariants = {
      record::InvariantRow{.record = "struct vec",
                           .field = "items",
                           .templ = "count(items) == cap + 0",
                           .verdict = core::Verdict::Violated,
                           .relied = false,
                           .store = at("a.c", 9, 5)}};
  ProgramMember reader = member("b.c");
  reader.payload.facts.invariants = {
      record::InvariantRow{.record = "struct vec",
                           .field = "items",
                           .templ = "count(items) == cap + 0",
                           .verdict = core::Verdict::Holds,
                           .relied = true,
                           .store = std::nullopt}};
  const DeclarationCheck check =
      verifyDeclarations(std::vector<ProgramMember>{writer, reader}, "/work");
  ASSERT_EQ(check.diagnostics.size(), 1U);
  EXPECT_EQ(check.diagnostics[0].message,
            "store to 'struct vec.items' breaks 'count(items) == cap + 0', "
            "which 'b.c' relies on");
  EXPECT_EQ(check.diagnostics[0].location, at("/work/a.c", 9, 5));
}

TEST(LinkStep, TheProgramLedgerCopiesTheRecordsFacetsOverTheRuns) {
  ProgramMember unit = member("a.c");
  unit.payload.sites = {record::FunctionRows{
      .function = "f",
      .file = "",
      .line = 1,
      .linkage = core::Linkage::External,
      .rows = {record::SiteRow{
          .ordinal = 0,
          .kind = core::SiteKind::Deref,
          .line = 10,
          .column = 3,
          .facets = {FacetDecision::checked(), FacetDecision::proven(),
                     FacetDecision::proven(), std::nullopt}}}}};
  core::Ledger run = runOf({ledgerFunction(
      "f", {{core::SiteKind::Deref, "", FacetDecision::proven()}},
      core::Linkage::External)});
  core::Site &site = run.units[0].functions[0].sites[0];
  site.addFacet(core::Facet::Spatial).decide(FacetDecision::proven());
  site.facet(core::Facet::Spatial)->decision.detail = "proven at link";
  site.addFacet(core::Facet::Null).decide(FacetDecision::proven());
  site.facet(core::Facet::Null)->decision.detail = "kept";
  const std::vector<ProgramMember> members{unit};
  const std::vector<const core::Ledger *> runs{&run};
  const core::Ledger ledger = composeProgramLedger(
      ProgramLedgerInput{.members = members, .runs = runs, .cwd = "/work"});
  EXPECT_EQ(ledger.scope, core::LedgerScope::Program);
  ASSERT_EQ(ledger.units.size(), 1U);
  EXPECT_EQ(ledger.units[0].source, "/work/a.c");
  EXPECT_EQ(ledger.units[0].object, "/work/a.c.o");
  const core::Site &composed = siteOf(ledger, 0, "f", 0);
  // The record's spatial outcome decided the code; the run's temporal one
  // is the program's.
  EXPECT_EQ(composed.facet(core::Facet::Spatial)->outcome(),
            core::SiteOutcome::Checked);
  EXPECT_EQ(composed.facet(core::Facet::Null)->decision.detail, "kept");
  EXPECT_EQ(composed.facet(core::Facet::Temporal)->outcome(),
            core::SiteOutcome::Proven);
  EXPECT_TRUE(core::completenessProblems(ledger).empty());
  // Without a run, the record's rows stand alone.
  const std::vector<const core::Ledger *> none{nullptr};
  const core::Ledger compact = composeProgramLedger(
      ProgramLedgerInput{.members = members, .runs = none, .cwd = "/work"});
  EXPECT_EQ(siteOf(compact, 0, "f", 0).facet(core::Facet::Spatial)->outcome(),
            core::SiteOutcome::Checked);
}

TEST(LinkStep, CrossUnitCallsWithoutASingleArgumentGetACallRow) {
  // §7.3: `use` relies on its parameter's Single default; `main` passes a
  // cursor, which is not Single-valid.
  ProgramMember caller = member("main.c");
  caller.payload.exports.imports = {"use"};
  caller.payload.facts.imports["use"] = record::ImportInterface{
      .declared = {},
      .location = at("main.c", 1, 6),
      .calls = {
          record::ImportCall{.function = "main", .site = 0, .args = {false}},
          record::ImportCall{.function = "main", .site = 1, .args = {true}}}};
  caller.payload.sites = {record::FunctionRows{
      .function = "main",
      .file = "",
      .line = 3,
      .linkage = core::Linkage::External,
      .rows = {
          record::SiteRow{.ordinal = 0,
                          .kind = core::SiteKind::Call,
                          .line = 5,
                          .column = 3,
                          .facets = {std::nullopt, std::nullopt,
                                     FacetDecision::proven(), std::nullopt}},
          record::SiteRow{.ordinal = 1,
                          .kind = core::SiteKind::Call,
                          .line = 6,
                          .column = 3,
                          .facets = {std::nullopt, std::nullopt,
                                     FacetDecision::proven(), std::nullopt}}}}};
  ProgramMember callee = member("use.c");
  define(callee, "use", core::FunctionSummary{},
         record::FunctionInterface{.params = {"default single nullable"},
                                   .result = std::nullopt,
                                   .reliesOnSingle = {0},
                                   .requirements = {},
                                   .location = std::nullopt});
  const std::vector<ProgramMember> members{caller, callee};
  const std::vector<const core::Ledger *> runs{nullptr, nullptr};
  const core::Ledger ledger = composeProgramLedger(
      ProgramLedgerInput{.members = members, .runs = runs, .cwd = "/work"});
  const core::FacetRecord *spatial =
      siteOf(ledger, 0, "main", 0).facet(core::Facet::Spatial);
  ASSERT_NE(spatial, nullptr);
  EXPECT_EQ(spatial->decision.compact(), "unresolved/unknown-extent");
  EXPECT_EQ(siteOf(ledger, 0, "main", 1).facet(core::Facet::Spatial), nullptr);
  ASSERT_TRUE(ledger.assumptions);
  EXPECT_EQ(ledger.assumptions->a1.reliesOnSingle, 1U);
  EXPECT_EQ(ledger.assumptions->a1.unverifiedCallers, 1U);
}

TEST(LinkStep, FacetsRestingOnAContradictedDeclarationProveNothing) {
  // `helper` calls the lying `inspect`, `main` calls `helper`; `other` calls
  // neither and keeps its proofs.
  ProgramMember caller = member("main.c");
  core::Ledger run = runOf(
      {ledgerFunction(
           "helper",
           {{core::SiteKind::Deref, "", FacetDecision::proven()},
            {core::SiteKind::Call, "inspect", FacetDecision::proven()}},
           core::Linkage::External),
       ledgerFunction(
           "main",
           {{core::SiteKind::Call, "helper", FacetDecision::proven()},
            {core::SiteKind::Deref, "",
             FacetDecision::trustedFor(core::TrustReason::Unsafe)}},
           core::Linkage::External),
       ledgerFunction("other",
                      {{core::SiteKind::Deref, "", FacetDecision::proven()}},
                      core::Linkage::Internal)});
  ProgramMember callee = member("impl.c");
  const DeclarationCheck check{
      .diagnostics = {},
      .contradictions = {Contradiction{.member = 0,
                                       .definer = 1,
                                       .function = "inspect",
                                       .detail = "the declaration lies"}}};
  const std::vector<ProgramMember> members{caller, callee};
  const std::vector<const core::Ledger *> runs{&run, nullptr};
  const core::Ledger ledger =
      composeProgramLedger(ProgramLedgerInput{.members = members,
                                              .runs = runs,
                                              .declarations = &check,
                                              .cwd = "/work"});
  for (const auto &[function, ordinal] :
       std::vector<std::pair<std::string, std::uint32_t>>{
           {"helper", 0}, {"helper", 1}, {"main", 0}, {"main", 1}}) {
    const core::FacetRecord *temporal =
        siteOf(ledger, 0, function, ordinal).facet(core::Facet::Temporal);
    ASSERT_NE(temporal, nullptr);
    EXPECT_EQ(temporal->decision.compact(), "unresolved/unknown-callee")
        << function << " " << ordinal;
    EXPECT_EQ(temporal->decision.detail, "the declaration lies");
  }
  EXPECT_EQ(
      siteOf(ledger, 0, "other", 0).facet(core::Facet::Temporal)->outcome(),
      core::SiteOutcome::Proven);
}

TEST(LinkStep, CallsIntoInputsWithoutRecordsAreTrusted) {
  ProgramMember unit = member("main.c");
  unit.payload.exports.imports = {"ext", "known"};
  core::Ledger run = runOf({ledgerFunction(
      "main",
      {{core::SiteKind::Call, "ext",
        FacetDecision::unresolvedFor(core::UnresolvedReason::UnknownCallee)},
       {core::SiteKind::Call, "known",
        FacetDecision::unresolvedFor(core::UnresolvedReason::UnknownCallee)}},
      core::Linkage::External)});
  ProgramMember other = member("known.c");
  define(other, "known", core::FunctionSummary{});
  const std::vector<ProgramMember> members{unit, other};
  const std::vector<const core::Ledger *> runs{&run, nullptr};
  const core::Ledger ledger = composeProgramLedger(ProgramLedgerInput{
      .members = members,
      .runs = runs,
      .shape = LinkShape{.executable = true,
                         .exportDynamic = false,
                         .inputsWithoutRecords = {"libext.a"}},
      .cwd = "/work"});
  EXPECT_EQ(siteOf(ledger, 0, "main", 0)
                .facet(core::Facet::Temporal)
                ->decision.compact(),
            "trusted/external-unit");
  // A callee some record defines stays as the run decided it.
  EXPECT_EQ(siteOf(ledger, 0, "main", 1)
                .facet(core::Facet::Temporal)
                ->decision.compact(),
            "unresolved/unknown-callee");
  EXPECT_EQ(ledger.assumptions->a3.inputsWithoutRecords,
            std::vector<std::string>{"libext.a"});
}

TEST(LinkStep, AUnitThatDefinesTheAllocatorIsNamed) {
  ProgramMember allocator = member("alloc.c");
  allocator.payload.facts.allocator = "malloc";
  ProgramMember user = member("main.c");
  EXPECT_FALSE(allocatorDefinedBy(std::vector<ProgramMember>{allocator, user}));
  user.payload.facts.loweredAllocations = 2;
  const std::vector<ProgramMember> members{allocator, user};
  const std::optional<AllocatorFinding> finding = allocatorDefinedBy(members);
  ASSERT_TRUE(finding);
  EXPECT_EQ(finding->member, 0U);
  EXPECT_EQ(allocatorWarning(members, *finding, "/work"),
            "heap zero-initialisation assumes the system allocator, but "
            "'alloc.c' defines 'malloc'; rebuild with -fno-weavec-zero-init");
  const std::vector<const core::Ledger *> runs{nullptr, nullptr};
  const core::Ledger ledger = composeProgramLedger(
      ProgramLedgerInput{.members = members, .runs = runs, .cwd = "/work"});
  EXPECT_EQ(ledger.assumptions->a5.allocatorDefinedBy,
            std::optional<std::string>("alloc.c"));
}

TEST(LinkStep, AssumptionsCountWhatTheRecordsCannotVerify) {
  ProgramMember a = member("a.c");
  define(a, "fill", core::FunctionSummary{},
         record::FunctionInterface{
             .params = {"default single nullable"},
             .result = std::nullopt,
             .reliesOnSingle = {},
             .requirements = {record::ExportedRequirement{
                 .param = 0,
                 .kind = "counted(param 1 scale 1 plus 0) nonnull",
                 .guard = std::nullopt}},
             .location = std::nullopt});
  a.payload.facts.slotKinds = {
      record::SlotKindRow{.slot = "field struct node next",
                          .kind = "single nullable",
                          .demotedBy = {}}};
  a.payload.facts.invariants = {
      record::InvariantRow{.record = "struct vec",
                           .field = "items",
                           .templ = "count(items) == cap + 0",
                           .verdict = core::Verdict::Unknown,
                           .relied = true,
                           .store = std::nullopt}};
  ProgramMember b = member("b.c");
  b.payload.facts.slotKinds = {
      record::SlotKindRow{.slot = "field struct node next",
                          .kind = "unknown nullable",
                          .demotedBy = {at("b.c", 3, 4)}}};
  const std::vector<ProgramMember> members{a, b};
  const std::vector<const core::Ledger *> runs{nullptr, nullptr};
  const core::Ledger ledger = composeProgramLedger(
      ProgramLedgerInput{.members = members, .runs = runs, .cwd = "/work"});
  ASSERT_TRUE(ledger.assumptions);
  const core::Assumptions &assumptions = *ledger.assumptions;
  EXPECT_EQ(assumptions.a1.exportedRequirements, 1U);
  EXPECT_EQ(assumptions.a1.verified, 0U);
  EXPECT_EQ(assumptions.unverifiedRequirements(), 1U);
  EXPECT_EQ(assumptions.a3.headerInvariants, 1U);
  // The unknown verdict, and the slot Single in one unit and demoted in the
  // other.
  EXPECT_EQ(assumptions.a3.unverified, 2U);
  EXPECT_EQ(core::summaryLine(ledger, "prog"),
            "weavec: program prog: 0 sites in 2 units: 0 proven, 0 checked, "
            "0 unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 1 "
            "exported requirement (A1), 2 header invariants (A3)");
}

} // namespace weavec::frontend
