//===- LedgerTest.cpp - Tests for the analysis's ledger model -------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Core/Ledger.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace weavec::core {

TEST(Ledger, Spellings) {
  const std::vector<std::pair<SiteKind, std::string_view>> kinds{
      {SiteKind::Deref, "deref"},         {SiteKind::Index, "index"},
      {SiteKind::PtrArith, "ptr-arith"},  {SiteKind::Cast, "cast"},
      {SiteKind::IntToPtr, "int-to-ptr"}, {SiteKind::LibCall, "lib-call"},
      {SiteKind::Release, "release"},     {SiteKind::Call, "call"},
      {SiteKind::Assume, "assume"},       {SiteKind::Raw, "raw"}};
  for (const auto &[kind, spelling] : kinds)
    EXPECT_EQ(toString(kind), spelling);
  EXPECT_EQ(toString(Facet::Spatial), "spatial");
  EXPECT_EQ(toString(Facet::Null), "null");
  EXPECT_EQ(toString(Facet::Temporal), "temporal");
  EXPECT_EQ(toString(Facet::Assertion), "assertion");
  // RFC 0035 §8: the analysis's four outcomes.
  EXPECT_EQ(toString(SiteOutcome::Proven), "proven");
  EXPECT_EQ(toString(SiteOutcome::Violation), "violation");
  EXPECT_EQ(toString(SiteOutcome::Unresolved), "unresolved");
  EXPECT_EQ(toString(SiteOutcome::Trusted), "trusted");
  EXPECT_EQ(toString(UnresolvedReason::UnknownExtent), "unknown-extent");
  EXPECT_EQ(toString(UnresolvedReason::Unconfirmed), "unconfirmed");
  EXPECT_EQ(toString(UnresolvedReason::Undecided), "undecided");
  EXPECT_EQ(toString(TrustReason::CallerContract), "caller-contract");
  EXPECT_EQ(toString(TrustReason::Concurrency), "concurrency");
}

TEST(Ledger, RankOrdersOutcomes) {
  // violation > unresolved > trusted > proven.
  EXPECT_GT(outcomeRank(SiteOutcome::Violation),
            outcomeRank(SiteOutcome::Unresolved));
  EXPECT_GT(outcomeRank(SiteOutcome::Unresolved),
            outcomeRank(SiteOutcome::Trusted));
  EXPECT_GT(outcomeRank(SiteOutcome::Trusted),
            outcomeRank(SiteOutcome::Proven));
  EXPECT_EQ(maxByRank(SiteOutcome::Trusted, SiteOutcome::Unresolved),
            SiteOutcome::Unresolved);
  EXPECT_EQ(maxByRank(SiteOutcome::Violation, SiteOutcome::Unresolved),
            SiteOutcome::Violation);
}

TEST(Ledger, DecisionsCarryReasonsOnlyWhereTheyBelong) {
  EXPECT_TRUE(FacetDecision::proven().isWellFormed());
  EXPECT_TRUE(FacetDecision::violation("x").isWellFormed());
  EXPECT_TRUE(
      FacetDecision::unresolvedFor(UnresolvedReason::Budget).isWellFormed());
  EXPECT_TRUE(FacetDecision::trustedFor(TrustReason::Unsafe).isWellFormed());
  EXPECT_FALSE(
      FacetDecision{.outcome = SiteOutcome::Unresolved}.isWellFormed());
  EXPECT_FALSE((FacetDecision{.outcome = SiteOutcome::Proven,
                              .trusted = TrustReason::Unsafe}
                    .isWellFormed()));
  EXPECT_EQ(FacetDecision::proven().reasonText(), "");
  EXPECT_EQ(
      FacetDecision::unresolvedFor(UnresolvedReason::MayReleased).reasonText(),
      "may-released");
  EXPECT_EQ(FacetDecision::trustedFor(TrustReason::SystemApi).reasonText(),
            "system-api");
}

TEST(Ledger, RecordsMergeByRank) {
  FacetRecord record;
  EXPECT_FALSE(record.decided);
  record.decide(FacetDecision::proven());
  EXPECT_TRUE(record.decided);
  EXPECT_EQ(record.outcome(), SiteOutcome::Proven);
  record.decide(FacetDecision::trustedFor(TrustReason::Unsafe));
  EXPECT_EQ(record.outcome(), SiteOutcome::Trusted);
  record.decide(FacetDecision::proven());
  EXPECT_EQ(record.outcome(), SiteOutcome::Trusted);
  record.decide(
      FacetDecision::unresolvedFor(UnresolvedReason::MayReleased, "path 1"));
  EXPECT_EQ(record.decision.unresolved, UnresolvedReason::MayReleased);
  // Equal rank keeps the earlier record.
  record.decide(
      FacetDecision::unresolvedFor(UnresolvedReason::UnknownCallee, "path 2"));
  EXPECT_EQ(record.decision.unresolved, UnresolvedReason::MayReleased);
  EXPECT_EQ(record.decision.detail, "path 1");
  record.decide(FacetDecision::violation("definite"));
  EXPECT_EQ(record.outcome(), SiteOutcome::Violation);
  EXPECT_FALSE(record.decision.unresolved);
  EXPECT_EQ(record.decision.detail, "definite");
}

TEST(Ledger, EqualRecordsMayFillAMissingDetail) {
  FacetRecord record;
  record.decide(FacetDecision::unresolvedFor(UnresolvedReason::Setjmp));
  record.decide(FacetDecision::unresolvedFor(UnresolvedReason::Setjmp,
                                             "'f' calls setjmp"));
  EXPECT_EQ(record.decision.detail, "'f' calls setjmp");
}

TEST(Ledger, ResetForgetsDecisionsButKeepsTheFacet) {
  Site site{.ordinal = 0, .kind = SiteKind::Deref};
  site.addFacet(Facet::Null)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::Undecided));
  FunctionLedger function{.name = "f", .sites = {site}};
  function.resetDecisions();
  const FacetRecord *null = function.sites[0].facet(Facet::Null);
  ASSERT_NE(null, nullptr);
  EXPECT_FALSE(null->decided);
  EXPECT_FALSE(function.sites[0].hasFacet(Facet::Spatial));
}

TEST(Ledger, DefaultsOfUndecidedFacets) {
  // §2.6, RFC 0035 §8: no decision was published.
  EXPECT_EQ(defaultDecision(false),
            FacetDecision::unresolvedFor(UnresolvedReason::Unanalysed));
  EXPECT_EQ(defaultDecision(true),
            FacetDecision::unresolvedFor(UnresolvedReason::Budget));
}

TEST(Ledger, SiteTextDropsWhitespaceAndTruncates) {
  EXPECT_EQ(siteText("item -> next"), "item->next");
  EXPECT_EQ(siteText(" a\t+\n b [ i ] "), "a+b[i]");
  const std::string longText(100, 'x');
  EXPECT_EQ(siteText(longText), std::string(80, 'x'));
  // A two-byte sequence straddling byte 80 is dropped whole.
  const std::string straddling = std::string(79, 'a') + "\xC3\xA9" + "zz";
  EXPECT_EQ(siteText(straddling), std::string(79, 'a'));
  const std::string fitting = std::string(78, 'a') + "\xC3\xA9" + "zz";
  EXPECT_EQ(siteText(fitting), std::string(78, 'a') + "\xC3\xA9");
}

TEST(Ledger, SiteOutcomeIsTheHighestRankedFacet) {
  Site site{.kind = SiteKind::Deref};
  EXPECT_EQ(site.outcome(), SiteOutcome::Proven);
  site.addFacet(Facet::Spatial).decide(FacetDecision::proven());
  site.addFacet(Facet::Null)
      .decide(FacetDecision::trustedFor(TrustReason::Concurrency));
  EXPECT_EQ(site.outcome(), SiteOutcome::Trusted);
  site.addFacet(Facet::Temporal)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::MayReleased));
  EXPECT_EQ(site.outcome(), SiteOutcome::Unresolved);
}

/// Two functions whose facets cover every outcome.
static Ledger sampleLedger() {
  Ledger ledger;
  UnitLedger unit{.source = "/work/a.c"};

  FunctionLedger f{.name = "f", .line = 1};
  Site deref{.ordinal = 0, .kind = SiteKind::Deref, .text = "p->x"};
  deref.addFacet(Facet::Null)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::Undecided));
  deref.addFacet(Facet::Spatial).decide(FacetDecision::proven());
  deref.addFacet(Facet::Temporal)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::UnknownCallee));
  Site index{.ordinal = 1, .kind = SiteKind::Index, .text = "b[i]"};
  index.addFacet(Facet::Spatial)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::Undecided));
  Site copy{.ordinal = 2, .kind = SiteKind::LibCall, .callee = "memcpy"};
  copy.addFacet(Facet::Spatial)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::UnknownExtent));
  copy.addFacet(Facet::Null).decide(FacetDecision::proven());
  copy.addFacet(Facet::Temporal)
      .decide(FacetDecision::trustedFor(TrustReason::LibrarySpec));
  f.sites = {deref, index, copy};

  FunctionLedger g{.name = "g", .line = 10, .overBudget = true};
  Site assume{.ordinal = 0, .kind = SiteKind::Assume};
  assume.addFacet(Facet::Assertion)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::Undecided));
  Site exit{.ordinal = 1, .kind = SiteKind::Call, .boundary = Boundary::Exit};
  exit.addFacet(Facet::Temporal)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::Budget));
  Site raw{.ordinal = 2, .kind = SiteKind::Raw};
  for (const Facet facet : {Facet::Spatial, Facet::Null, Facet::Temporal})
    raw.addFacet(facet).decide(FacetDecision::trustedFor(TrustReason::Unsafe));
  Site null{.ordinal = 3, .kind = SiteKind::Deref};
  null.addFacet(Facet::Null).decide(FacetDecision::violation());
  null.addFacet(Facet::Spatial).decide(FacetDecision::proven());
  null.addFacet(Facet::Temporal).decide(FacetDecision::proven());
  g.sites = {assume, exit, raw, null};

  unit.functions = {f, g};
  ledger.units.push_back(unit);
  ledger.diagnostics.push_back(LedgerDiagnostic{
      .id = "null-dereference", .severity = Severity::Error, .function = "g"});
  ledger.diagnostics.push_back(
      LedgerDiagnostic{.id = "use-after-free",
                       .severity = Severity::Warning,
                       .certainty = Certainty::Possible});
  return ledger;
}

TEST(Ledger, RollupCountsSitesAndFindings) {
  const Ledger ledger = sampleLedger();
  const LedgerSummary summary = summarize(ledger);
  EXPECT_EQ(summary.sites, 7U);
  EXPECT_EQ(summary.outcomes,
            (OutcomeCounts{
                .proven = 0, .violation = 1, .unresolved = 5, .trusted = 1}));
  EXPECT_EQ(summary.errors, 1U);
  EXPECT_EQ(summary.warnings, 1U);
  EXPECT_EQ(summary.overBudget, std::vector<std::string>{"g"});
  EXPECT_EQ(summary.sites, summary.outcomes.total());
}

TEST(Ledger, CompletenessProblemsAreNamed) {
  Ledger ledger = sampleLedger();
  EXPECT_TRUE(completenessProblems(ledger).empty());
  Site &site = ledger.units[0].functions[0].sites[1];
  site.addFacet(Facet::Null);
  site.facet(Facet::Spatial)->diagnostic = 9;
  ledger.units[0].functions[1].sites[3].ordinal = 7;
  const std::vector<std::string> problems = completenessProblems(ledger);
  ASSERT_EQ(problems.size(), 3U);
  EXPECT_NE(problems[0].find("links to missing diagnostic 9"),
            std::string::npos);
  EXPECT_NE(problems[1].find("null facet is undecided"), std::string::npos);
  EXPECT_NE(problems[2].find("ordinal 7"), std::string::npos);
}

TEST(Ledger, ThousandsSeparators) {
  EXPECT_EQ(formatThousands(0), "0");
  EXPECT_EQ(formatThousands(999), "999");
  EXPECT_EQ(formatThousands(1000), "1,000");
  EXPECT_EQ(formatThousands(4210), "4,210");
  EXPECT_EQ(formatThousands(1234567), "1,234,567");
}

static LedgerSummary cjsonSummary() {
  LedgerSummary summary;
  summary.sites = 4210;
  summary.outcomes = OutcomeCounts{
      .proven = 3050, .violation = 0, .unresolved = 1130, .trusted = 30};
  summary.warnings = 2;
  return summary;
}

TEST(Ledger, UnitSummaryLine) {
  // RFC 0035 §8.
  EXPECT_EQ(unitSummaryLine("cJSON.c", cjsonSummary()),
            "weavec: cJSON.c: 4,210 sites: 3,050 proven, 1,130 not proven, 0 "
            "violations, 30 trusted; 0 errors, 2 warnings");
  LedgerSummary overBudget = cjsonSummary();
  overBudget.overBudget = {"cJSON_ParseWithLengthOpts"};
  EXPECT_EQ(unitSummaryLine("cJSON.c", overBudget),
            "weavec: cJSON.c: 4,210 sites: 3,050 proven, 1,130 not proven, 0 "
            "violations, 30 trusted; 0 errors, 2 warnings; 1 function over "
            "budget (cJSON_ParseWithLengthOpts)");
  overBudget.overBudget.emplace_back("print_value");
  EXPECT_NE(unitSummaryLine("cJSON.c", overBudget)
                .find("; 2 functions over budget (cJSON_ParseWithLengthOpts, "
                      "print_value)"),
            std::string::npos);
}

TEST(Ledger, SummaryLineListsViolationsAndSingulars) {
  LedgerSummary summary;
  summary.sites = 1;
  summary.outcomes.violation = 1;
  summary.errors = 1;
  summary.warnings = 1;
  EXPECT_EQ(unitSummaryLine("a.c", summary),
            "weavec: a.c: 1 site: 0 proven, 0 not proven, 1 violation, 0 "
            "trusted; 1 error, 1 warning");
}

TEST(Ledger, ProgramSummaryLine) {
  LedgerSummary summary;
  summary.sites = 9876;
  summary.outcomes = OutcomeCounts{
      .proven = 9000, .violation = 0, .unresolved = 870, .trusted = 6};
  summary.warnings = 1;
  EXPECT_EQ(programSummaryLine("minigzip", 3, summary),
            "weavec: program minigzip: 9,876 sites in 3 units: 9,000 proven, "
            "870 not proven, 0 violations, 6 trusted; 0 errors, 1 warning");
  EXPECT_EQ(programSummaryLine("p", 1, LedgerSummary{}),
            "weavec: program p: 0 sites in 1 unit: 0 proven, 0 not proven, 0 "
            "violations, 0 trusted; 0 errors, 0 warnings");
}

TEST(Ledger, SummaryLineOfALedger) {
  const Ledger ledger = sampleLedger();
  EXPECT_EQ(unitSummaryLine("a.c", summarize(ledger)),
            "weavec: a.c: 7 sites: 0 proven, 5 not proven, 1 violation, 1 "
            "trusted; 1 error, 1 warning; 1 function over budget (g)");
}

TEST(Ledger, SitesAreFoundById) {
  Ledger ledger = sampleLedger();
  const UnitLedger &unit = ledger.units[0];
  const Site *site = unit.site(SiteId{.function = 1, .ordinal = 2});
  ASSERT_NE(site, nullptr);
  EXPECT_EQ(site->kind, SiteKind::Raw);
  EXPECT_EQ(unit.site(SiteId{.function = 2, .ordinal = 0}), nullptr);
  EXPECT_EQ(unit.site(SiteId{.function = 0, .ordinal = 9}), nullptr);
  EXPECT_LT((SiteId{.function = 0, .ordinal = 5}),
            (SiteId{.function = 1, .ordinal = 0}));
}

TEST(Ledger, DiagnosticsSortIntoSourceOrderKeepingLinks) {
  Ledger ledger = sampleLedger();
  const auto at = [](std::uint32_t line, std::uint32_t column) {
    return SourceLocation{
        .file = "a.c", .line = line, .column = column, .opaque = 0};
  };
  ledger.diagnostics = {LedgerDiagnostic{.id = "b", .location = at(9, 1)},
                        LedgerDiagnostic{.id = "c", .location = at(2, 5)},
                        LedgerDiagnostic{.id = "a", .location = at(2, 5)}};
  FacetRecord *linked =
      ledger.units[0].functions[1].sites[3].facet(Facet::Null);
  linked->diagnostic = 0;
  sortDiagnostics(ledger);
  ASSERT_EQ(ledger.diagnostics.size(), 3U);
  EXPECT_EQ(ledger.diagnostics[0].id, "a");
  EXPECT_EQ(ledger.diagnostics[1].id, "c");
  EXPECT_EQ(ledger.diagnostics[2].id, "b");
  EXPECT_EQ(linked->diagnostic, 2U);
  EXPECT_TRUE(completenessProblems(ledger).empty());
}

} // namespace weavec::core
