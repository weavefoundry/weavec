//===- LedgerAdapterTest.cpp - Tests for LedgerAdapter --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §14: defaults (§2.6), merging (§2.5), discarding passes, the
// UNSAFE and setjmp rules, diagnostics and their links, and the require-level
// errors (§6.3).
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/LedgerAdapter.h"

#include "SiteTestUtils.h"
#include "weavec/Analysis/ClangLocation.h"
#include "weavec/Analysis/UnitPipeline.h"

#include "clang/AST/Expr.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace weavec::analysis {

using test::collectUnit;
using Lines = std::vector<std::string>;

/// `<text> <facet>=<outcome>[/<reason>]...` for every site of `function`.
static Lines outcomes(const core::Ledger &ledger, llvm::StringRef function) {
  Lines out;
  for (const core::FunctionLedger &row : ledger.units.front().functions) {
    if (row.name != function)
      continue;
    for (const core::Site &site : row.sites) {
      std::string line = site.text;
      for (const core::Facet facet : core::AllFacets)
        if (const core::FacetRecord *record = site.facet(facet))
          line += " " + std::string(core::toString(facet)) + "=" +
                  record->decision.compact();
      out.push_back(line);
    }
  }
  return out;
}

static const SiteInfo &siteOf(const test::CollectedUnit &unit,
                              llvm::StringRef function, std::uint32_t ordinal) {
  return unit.sites.function(*unit.function(function))->sites.at(ordinal);
}

/// The pipeline over `unit`: its diagnostics, as `<line>: <severity>:
/// <message> [<notes>]`, and its ledger.
namespace {
struct Piped {
  Lines diagnostics;
  core::Ledger ledger;
};
} // namespace
static Piped pipe(const test::CollectedUnit &unit) {
  core::DiagnosticCollector collected;
  const UnitPipelineResult result =
      runUnitAnalysis(unit.context(), UnitPipelineOptions{}, collected);
  Piped out;
  for (const core::Diagnostic &d : collected.diagnostics()) {
    std::string line = std::to_string(d.location.line) + ": " +
                       std::string(core::toString(d.severity)) + ": " +
                       d.message;
    for (const core::Diagnostic &note : d.notes)
      line += " [" + note.message + "]";
    out.diagnostics.push_back(std::move(line));
  }
  if (result.ledger)
    out.ledger = result.ledger->ledger;
  return out;
}

namespace {

// §2.6: the defaults of undecided facets, and of an over-budget function.
TEST(LedgerAdapter, FillsDefaults) {
  const auto unit = collectUnit(R"c(
int f(int *p, int i, unsigned long a) {
  int arr[4] = {0};
  ASSUME(i < 4);
  return *p + arr[i] + p[i] + *(int *)a;
}
int g(int *p) { return *p; }
)c");
  LedgerAdapterOptions options;
  options.source = "input.c";
  LedgerAdapter adapter(unit.context(), unit.sites, options);
  adapter.overBudget(*unit.function("g"));
  const PlannedLedger planned = adapter.finish();
  EXPECT_TRUE(core::completenessProblems(planned.ledger).empty());
  EXPECT_EQ(
      outcomes(planned.ledger, "f"),
      (Lines{
          "ASSUME(i<4) assertion=checked",
          std::string("return*p+arr[i]+p[i]+*(int*)a ") +
              "temporal=unresolved/unanalysed",
          std::string("*p spatial=unresolved/unanalysed null=checked ") +
              "temporal=unresolved/unanalysed",
          "arr[i] spatial=checked",
          std::string("p[i] spatial=unresolved/unanalysed null=checked ") +
              "temporal=unresolved/unanalysed",
          std::string("*(int*)a spatial=unresolved/unanalysed null=checked ") +
              "temporal=unresolved/unanalysed",
          std::string("(int*)a spatial=unresolved/raw-cast ") +
              "temporal=unresolved/raw-cast"}));
  EXPECT_EQ(outcomes(planned.ledger, "g"),
            (Lines{"return*p temporal=unresolved/budget",
                   std::string("*p spatial=unresolved/budget null=checked ") +
                       "temporal=unresolved/budget"}));
  const core::FunctionLedger &g = planned.ledger.units.front().functions.at(
      unit.sites.function(*unit.function("g"))->index);
  EXPECT_TRUE(g.overBudget);
  // A default `checked` facet gets its check planned: `arr[i]` against 4,
  // every nonnull and the assertion.
  EXPECT_EQ(planned.plan.size(), 6U);
  EXPECT_EQ(planned.ledger.config, core::LedgerConfig{});
  EXPECT_EQ(planned.ledger.units.front().source, "input.c");
}

// §2.5: records merge by rank within a pass; `beginFunction` discards an
// earlier pass's rows; a discarding adapter keeps nothing.
TEST(LedgerAdapter, MergesByRankAndDiscardsEarlierPasses) {
  const auto unit = collectUnit("int f(int *p) { return *p; }\n");
  const SiteInfo &deref = siteOf(unit, "f", 1);
  ASSERT_EQ(deref.kind, core::SiteKind::Deref);
  LedgerAdapter adapter(unit.context(), unit.sites);
  const auto temporal = [&] {
    return adapter.unitLedger()
        .functions.front()
        .sites[1]
        .facet(core::Facet::Temporal)
        ->decision.compact();
  };
  adapter.beginFunction(*unit.function("f"));
  adapter.decide(*deref.stmt, core::Facet::Temporal, core::SiteOutcome::Proven);
  EXPECT_EQ(temporal(), "proven");
  adapter.decide(*deref.stmt, core::Facet::Temporal,
                 core::SiteOutcome::Unresolved,
                 core::UnresolvedReason::MayAliasReleased);
  EXPECT_EQ(temporal(), "unresolved/may-alias-released");
  // Equal rank keeps the earlier record.
  adapter.decide(*deref.stmt, core::Facet::Temporal,
                 core::SiteOutcome::Unresolved,
                 core::UnresolvedReason::UnknownCallee);
  EXPECT_EQ(temporal(), "unresolved/may-alias-released");
  adapter.decide(*deref.stmt, core::Facet::Temporal,
                 core::SiteOutcome::Violation);
  EXPECT_EQ(temporal(), "violation");
  // A new authoritative pass starts from scratch.
  adapter.beginFunction(*unit.function("f"));
  adapter.decide(*deref.stmt, core::Facet::Temporal, core::SiteOutcome::Proven);
  EXPECT_EQ(temporal(), "proven");

  LedgerAdapter discarding(unit.context(), unit.sites, {},
                           LedgerAdapter::Mode::Discarding);
  EXPECT_TRUE(discarding.isDiscarding());
  discarding.beginFunction(*unit.function("f"));
  discarding.decide(*deref.stmt, core::Facet::Temporal,
                    core::SiteOutcome::Violation);
  discarding.report(core::Diagnostic{.severity = core::Severity::Error,
                                     .id = core::diag::UseAfterFree,
                                     .message = "x",
                                     .location = {},
                                     .notes = {},
                                     .fixits = {}},
                    core::Certainty::Definite);
  EXPECT_TRUE(discarding.diagnostics().empty());
  EXPECT_TRUE(discarding.finish().ledger.units.empty());
}

// Diagnostics keep their order and link to the site and facet they are
// about.
TEST(LedgerAdapter, ReportsAndLinksDiagnostics) {
  const auto unit = collectUnit("int f(int *p) { return *p; }\n");
  const SiteInfo &deref = siteOf(unit, "f", 1);
  LedgerAdapter adapter(unit.context(), unit.sites);
  adapter.beginFunction(*unit.function("f"));
  const core::SourceLocation at = toCoreLocation(
      unit.context().getSourceManager(), deref.stmt->getBeginLoc());
  core::Diagnostic second{.severity = core::Severity::Warning,
                          .id = core::diag::Leak,
                          .message = "second",
                          .location = at,
                          .notes = {},
                          .fixits = {}};
  core::Diagnostic first{.severity = core::Severity::Error,
                         .id = core::diag::UseAfterFree,
                         .message = "use of 'p' after it was freed",
                         .location = at,
                         .notes = {},
                         .fixits = {}};
  first.addNote("freed here", at);
  adapter.report(first, core::Certainty::Definite, deref.stmt,
                 core::Facet::Temporal);
  adapter.decide(*deref.stmt, core::Facet::Temporal,
                 core::SiteOutcome::Violation);
  adapter.report(second, core::Certainty::Possible);
  ASSERT_EQ(adapter.diagnostics().size(), 2U);
  EXPECT_EQ(adapter.diagnostics()[0].message, "use of 'p' after it was freed");
  EXPECT_EQ(adapter.diagnostics()[1].message, "second");
  const PlannedLedger planned = adapter.finish();
  ASSERT_EQ(planned.ledger.diagnostics.size(), 2U);
  const core::LedgerDiagnostic &linked = planned.ledger.diagnostics[1];
  EXPECT_EQ(linked.id, "use-after-free");
  EXPECT_EQ(linked.function, "f");
  EXPECT_EQ(linked.site, std::optional<std::uint32_t>(1));
  EXPECT_EQ(linked.facet, std::optional(core::Facet::Temporal));
  ASSERT_EQ(linked.notes.size(), 1U);
  EXPECT_EQ(planned.ledger.diagnostics[0].function, "f");
  EXPECT_EQ(planned.ledger.diagnostics[0].certainty, core::Certainty::Possible);
  // Sorting kept the facet's link on the right diagnostic.
  const core::FacetRecord *temporal =
      planned.ledger.units.front().functions.front().sites[1].facet(
          core::Facet::Temporal);
  EXPECT_EQ(temporal->diagnostic, std::optional<std::uint32_t>(1));
  EXPECT_TRUE(core::completenessProblems(planned.ledger).empty());
}

// §2.5: requirement records keep their own outcomes; a checked one is
// planned even when the merged facet is unresolved.
TEST(LedgerAdapter, RequirementRecords) {
  const auto unit = collectUnit(R"c(
void f(char *d, const char *s, unsigned long n) { memcpy(d, s, n); }
)c");
  const SiteInfo &copy = siteOf(unit, "f", 0);
  ASSERT_EQ(copy.kind, core::SiteKind::LibCall);
  LedgerAdapter adapter(unit.context(), unit.sites);
  adapter.beginFunction(*unit.function("f"));
  adapter.requirement(
      *copy.stmt, core::Facet::Spatial,
      core::Requirement{.argument = 0,
                        .decision = core::FacetDecision::checked()},
      CheckWitness{.shape = CheckWitness::Shape::Length,
                   .argument = 0,
                   .extent = WitnessTerm::ofConstant(16),
                   .extentClass = core::ExtentClass::Exact,
                   .need = WitnessTerm::ofConstant(8),
                   .unmodified = true,
                   .accessesSafe = true});
  adapter.requirement(
      *copy.stmt, core::Facet::Spatial,
      core::Requirement{.argument = 1,
                        .decision = core::FacetDecision::unresolvedFor(
                            core::UnresolvedReason::UnknownExtent)});
  const PlannedLedger planned = adapter.finish();
  const core::FacetRecord *spatial =
      planned.ledger.units.front().functions.front().sites[0].facet(
          core::Facet::Spatial);
  EXPECT_EQ(spatial->decision.compact(), "unresolved/unknown-extent");
  ASSERT_EQ(spatial->requirements.size(), 2U);
  EXPECT_TRUE(spatial->requirements[0].check.has_value());
  EXPECT_FALSE(spatial->requirements[1].check.has_value());
  const auto len =
      std::count_if(planned.plan.entries.begin(), planned.plan.entries.end(),
                    [](const core::CheckPlanEntry &entry) {
                      return entry.kind == core::CheckPlanEntry::Template::Len;
                    });
  EXPECT_EQ(len, 1);
}

// §6.1 and §5.4, as ledger-side rules.
TEST(LedgerAdapter, UnsafeAndSetjmpRules) {
  const auto unit = collectUnit(R"c(
jmp_buf env;
int f(int *p, int *RAW r) {
  UNSAFE { *p = 1; *r = 2; ASSUME(*p == 1); }
  return *p;
}
int g(int *p) {
  if (setjmp(env))
    return 0;
  return *p;
}
)c");
  LedgerAdapter adapter(unit.context(), unit.sites);
  // The engine proved `*p`'s null facet in `g` from flow facts.
  const SiteInfo &guarded = siteOf(unit, "g", 3);
  ASSERT_EQ(guarded.kind, core::SiteKind::Deref);
  adapter.beginFunction(*unit.function("g"));
  adapter.decide(*guarded.stmt, core::Facet::Null, core::SiteOutcome::Proven);
  const PlannedLedger planned = adapter.finish();
  EXPECT_EQ(
      outcomes(planned.ledger, "f"),
      (Lines{std::string("*p spatial=trusted/unsafe null=trusted/unsafe ") +
                 "temporal=unresolved/unanalysed",
             std::string("*r spatial=trusted/unsafe null=trusted/unsafe ") +
                 "temporal=trusted/unsafe",
             std::string("*p spatial=trusted/unsafe null=trusted/unsafe ") +
                 "temporal=unresolved/unanalysed",
             "ASSUME(*p==1) assertion=checked",
             "return*p temporal=unresolved/unanalysed",
             std::string("*p spatial=unresolved/unanalysed null=checked ") +
                 "temporal=unresolved/unanalysed"}));
  EXPECT_EQ(
      outcomes(planned.ledger, "g"),
      (Lines{"setjmp(env)", "return0 temporal=unresolved/setjmp",
             "return*p temporal=unresolved/setjmp",
             std::string("*p spatial=unresolved/unanalysed null=checked ") +
                 "temporal=unresolved/setjmp"}));
  EXPECT_TRUE(core::completenessProblems(planned.ledger).empty());
}

// §6.3: require levels make unresolved (and, at `proven`, checked) facets
// errors, after the engine's diagnostics.
TEST(LedgerAdapter, RequireLevels) {
  const auto unit = collectUnit(R"c(
char *get(void);
int f(void) { return get()[1000]; }
int g(int *p) { return *p; }
)c");
  LedgerAdapterOptions checked;
  checked.config.require = core::RequireLevel::Checked;
  LedgerAdapter adapter(unit.context(), unit.sites, checked);
  const PlannedLedger planned = adapter.finish();
  Lines messages;
  for (const core::Diagnostic &diagnostic : adapter.diagnostics())
    messages.push_back(std::string(diagnostic.id) + ": " + diagnostic.message);
  EXPECT_EQ(messages.size(), 7U);
  EXPECT_NE(
      std::ranges::find(
          messages,
          std::string(
              "unresolved-operation: access 'get()[1000]' is neither ") +
              std::string(
                  "proven nor checkable: WeaveC does not model this (no ") +
              "decision was published) [unanalysed]"),
      messages.end());
  EXPECT_NE(
      std::ranges::find(
          messages,
          std::string(
              "unresolved-operation: boundary of 'g' is neither proven ") +
              std::string(
                  "nor checkable: WeaveC does not model this (no decision ") +
              "was published) [unanalysed]"),
      messages.end());
  for (const core::LedgerDiagnostic &diagnostic : planned.ledger.diagnostics) {
    EXPECT_EQ(diagnostic.severity, core::Severity::Error);
    EXPECT_TRUE(diagnostic.site.has_value());
  }

  LedgerAdapterOptions proven;
  proven.config.require = core::RequireLevel::Proven;
  LedgerAdapter strict(unit.context(), unit.sites, proven);
  (void)strict.finish();
  Lines unchecked;
  for (const core::Diagnostic &diagnostic : strict.diagnostics())
    if (diagnostic.id == core::diag::UncheckedOperation)
      unchecked.push_back(diagnostic.message);
  EXPECT_EQ(
      unchecked,
      (Lines{std::string("access 'get()[1000]' relies on a runtime nonnull ") +
                 "check",
             std::string("dereference of 'p' relies on a runtime nonnull ") +
                 "check"}));

  LedgerAdapter none(unit.context(), unit.sites);
  (void)none.finish();
  EXPECT_TRUE(none.diagnostics().empty());
}

// §2.6 item 2: a decision about a statement no site stands for is an
// internal error: an assertion in debug builds, an `unresolved(unanalysed)`
// row otherwise.
TEST(LedgerAdapter, OrphanDecisions) {
  const auto unit = collectUnit("int f(int *p) { return p != 0; }\n");
  const auto *ret = llvm::cast<clang::ReturnStmt>(siteOf(unit, "f", 0).stmt);
  const clang::Expr *comparison = ret->getRetValue();
  EXPECT_DEBUG_DEATH(
      {
        LedgerAdapter adapter(unit.context(), unit.sites);
        adapter.beginFunction(*unit.function("f"));
        adapter.decide(*comparison, core::Facet::Null,
                       core::SiteOutcome::Proven);
        EXPECT_EQ(adapter.orphanDecisions(), 1U);
        const PlannedLedger planned = adapter.finish();
        EXPECT_EQ(outcomes(planned.ledger, "f").back(),
                  "p!=0 null=unresolved/unanalysed");
      },
      "did not enumerate");
}

// The unit pipeline reports the engine's diagnostics in the engine's order
// and returns a complete ledger.
TEST(UnitPipeline, ReportsEngineDiagnosticsAndBuildsTheLedger) {
  const auto unit = collectUnit(R"c(
int f(void) {
  int *p = malloc(sizeof *p);
  if (!p)
    return 0;
  free(p);
  return *p;
}
)c");
  core::DiagnosticCollector collected;
  UnitPipelineOptions options;
  const UnitPipelineResult result =
      runUnitAnalysis(unit.context(), options, collected);
  ASSERT_EQ(collected.size(), 1U);
  EXPECT_EQ(collected.diagnostics().front().id, core::diag::UseAfterFree);
  ASSERT_NE(result.ledger, nullptr);
  const core::Ledger &ledger = result.ledger->ledger;
  EXPECT_TRUE(core::completenessProblems(ledger).empty());
  ASSERT_EQ(ledger.diagnostics.size(), 1U);
  EXPECT_EQ(ledger.diagnostics.front().function, "f");
  EXPECT_EQ(ledger.diagnostics.front().facet,
            std::optional(core::Facet::Temporal));
  EXPECT_NE(result.ledger->sites, nullptr);
  EXPECT_TRUE(result.exports.functions.contains("f"));

  UnitPipelineOptions discovery;
  discovery.discoverOnly = true;
  core::DiagnosticCollector none;
  const UnitPipelineResult discovered =
      runUnitAnalysis(unit.context(), discovery, none);
  EXPECT_EQ(discovered.ledger, nullptr);
  EXPECT_TRUE(none.empty());
}

// -- RFC 0030 §3: certainty, severity and the engine's decisions ------------

TEST(EngineCertainty, TemporalFindingsAreDefiniteOrPossible) {
  const auto unit = collectUnit(R"c(
void g(char *p) { free(p); p[0] = 1; }
void h(char *p, int c) { if (c) free(p); p[0] = 1; }
void twice(char *p, int c) { if (c) free(p); free(p); }
void fine(char *p) { p[0] = 1; free(p); }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"2: error: use of 'p' after it was freed [freed here]",
                   "3: warning: use of 'p' after it may have been freed "
                   "[freed here on some paths]",
                   "4: warning: 'p' may be freed twice [previously freed here "
                   "on some paths]"}));
  EXPECT_EQ(outcomes(piped.ledger, "g")[1],
            "p[0] spatial=unresolved/unknown-extent null=checked "
            "temporal=violation");
  EXPECT_EQ(outcomes(piped.ledger, "h")[1],
            "p[0] spatial=unresolved/unknown-extent null=checked "
            "temporal=unresolved/may-released");
  // The release's own spatial facet is stage S3-B2's.
  EXPECT_EQ(outcomes(piped.ledger, "twice")[1],
            "free(p) spatial=unresolved/unanalysed "
            "temporal=unresolved/may-released");
  EXPECT_EQ(outcomes(piped.ledger, "fine"),
            (Lines{"p[0] spatial=unresolved/unknown-extent null=checked "
                   "temporal=proven",
                   "free(p) spatial=unresolved/unanalysed temporal=proven",
                   "} temporal=proven"}));
  ASSERT_EQ(piped.ledger.diagnostics.size(), 3U);
  EXPECT_EQ(piped.ledger.diagnostics[0].certainty, core::Certainty::Definite);
  EXPECT_EQ(piped.ledger.diagnostics[1].certainty, core::Certainty::Possible);
  EXPECT_EQ(piped.ledger.diagnostics[1].facet,
            std::optional(core::Facet::Temporal));
}

TEST(EngineCertainty, NullFindingsAreDefiniteOnly) {
  const auto unit = collectUnit(R"c(
struct node { int v; };
int k(struct node *n) { if (n == 0) return n->v; return 0; }
void m(void) { char *p = malloc(4); p[0] = 1; free(p); }
int q(char *p) { return p[0]; }
int r(void) { char *p = malloc(4); if (!p) return 0; int c = p[0]; free(p); return c; }
)c");
  const Piped piped = pipe(unit);
  // `allocation-failure` is off by default; the engine reports it and the
  // frontend drops it unless enabled.
  EXPECT_EQ(piped.diagnostics,
            (Lines{"3: error: dereference of 'n', which is null ['n' may be "
                   "null: it is compared with NULL here]",
                   "4: warning: the result of 'malloc' is used without a null "
                   "test; it is null when allocation fails [allocated here]"}));
  EXPECT_EQ(outcomes(piped.ledger, "k")[1],
            "n->v spatial=unresolved/unknown-extent null=violation "
            "temporal=proven");
  EXPECT_EQ(outcomes(piped.ledger, "m")[1],
            "p[0] spatial=proven null=checked temporal=proven");
  EXPECT_EQ(outcomes(piped.ledger, "q")[1],
            "p[0] spatial=unresolved/unknown-extent null=checked "
            "temporal=proven");
  EXPECT_EQ(outcomes(piped.ledger, "r")[2],
            "p[0] spatial=proven null=proven temporal=proven");
}

TEST(EngineCertainty, SpatialFindingsAreDefiniteOnlyAgainstExactExtents) {
  const auto unit = collectUnit(R"c(
void e(void) { char *p = malloc(4); if (!p) return; p[4] = 0; free(p); }
void loop(void) { char b[4]; for (int i = 0; i < 8; i++) b[i] = 0; }
void declared(char *SIZED_BY(n) p, size_t n) { p[n] = 0; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"2: error: 'p[4]' is out of bounds: index 4 of an object of "
                   "4 bytes ['p' is allocated here]"}));
  EXPECT_EQ(outcomes(piped.ledger, "e")[2],
            "p[4] spatial=violation null=proven temporal=proven");
  EXPECT_EQ(outcomes(piped.ledger, "loop")[0], "b[i] spatial=checked");
  EXPECT_EQ(outcomes(piped.ledger, "declared")[0],
            "p[n] spatial=checked null=checked temporal=proven");
}

TEST(EngineCertainty, ContextFindingsLinkToTheCall) {
  const auto unit = collectUnit(R"c(
static void two(char *a, char *b) { free(a); b[0] = 1; }
void caller(void) { char *p = malloc(4); if (!p) return; two(p, p); }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"2: error: use of 'b' after it was freed [freed here "
                   "(through 'a')] [called here with related pointer "
                   "arguments]"}));
  // The callee's own site is decided by its generic pass (RFC 0030 §3.1:
  // `may-alias-released` there is stage S3-B3's); the context's finding is
  // the call's.
  EXPECT_EQ(outcomes(piped.ledger, "two")[1],
            "b[0] spatial=unresolved/unknown-extent null=checked "
            "temporal=proven");
  EXPECT_EQ(outcomes(piped.ledger, "caller")[2], "two(p,p) temporal=violation");
  ASSERT_EQ(piped.ledger.diagnostics.size(), 1U);
  EXPECT_EQ(piped.ledger.diagnostics[0].function, "caller");
  EXPECT_EQ(piped.ledger.diagnostics[0].facet,
            std::optional(core::Facet::Temporal));
}

} // namespace
} // namespace weavec::analysis
