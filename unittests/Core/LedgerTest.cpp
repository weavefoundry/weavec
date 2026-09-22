//===- LedgerTest.cpp - Tests for the RFC 0030 ledger model ---------------===//
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

TEST(Ledger, SiteKindSpellingsFollowTheRfc) {
  const std::vector<std::pair<SiteKind, std::string_view>> expected{
      {SiteKind::Deref, "deref"},         {SiteKind::Index, "index"},
      {SiteKind::PtrArith, "ptr-arith"},  {SiteKind::Cast, "cast"},
      {SiteKind::IntToPtr, "int-to-ptr"}, {SiteKind::LibCall, "lib-call"},
      {SiteKind::Release, "release"},     {SiteKind::Call, "call"},
      {SiteKind::Assume, "assume"},       {SiteKind::Raw, "raw"}};
  ASSERT_EQ(expected.size(), SiteKindCount);
  for (const auto &[kind, spelling] : expected) {
    EXPECT_EQ(toString(kind), spelling);
    EXPECT_EQ(parseSiteKind(spelling), kind);
  }
  EXPECT_FALSE(parseSiteKind("Deref"));
  EXPECT_FALSE(parseSiteKind(""));
}

TEST(Ledger, FacetAndOutcomeSpellings) {
  EXPECT_EQ(toString(Facet::Spatial), "spatial");
  EXPECT_EQ(toString(Facet::Null), "null");
  EXPECT_EQ(toString(Facet::Temporal), "temporal");
  EXPECT_EQ(toString(Facet::Assertion), "assertion");
  for (const Facet facet : AllFacets)
    EXPECT_EQ(parseFacet(toString(facet)), facet);

  const std::vector<std::string_view> outcomes{"proven", "checked", "violation",
                                               "unresolved", "trusted"};
  for (std::size_t i = 0; i < SiteOutcomeCount; ++i) {
    EXPECT_EQ(toString(AllSiteOutcomes.at(i)), outcomes[i]);
    EXPECT_EQ(parseSiteOutcome(outcomes[i]), AllSiteOutcomes.at(i));
  }
  EXPECT_FALSE(parseSiteOutcome("unknown"));
}

TEST(Ledger, UnresolvedReasonsAreTheClosedListInOrder) {
  const std::vector<std::string_view> expected{"unknown-extent",
                                               "unknown-index",
                                               "inexpressible",
                                               "may-released",
                                               "may-moved",
                                               "may-alias-released",
                                               "may-invalid-release",
                                               "may-mismatched-release",
                                               "may-dangle",
                                               "may-conflict",
                                               "unknown-callee",
                                               "callback",
                                               "setjmp",
                                               "budget",
                                               "unanalysed",
                                               "raw-cast",
                                               "dangling-escape",
                                               "second-owner",
                                               "no-zero-init"};
  const auto all = allUnresolvedReasons();
  ASSERT_EQ(all.size(), UnresolvedReasonCount);
  ASSERT_EQ(expected.size(), UnresolvedReasonCount);
  for (std::size_t i = 0; i < all.size(); ++i) {
    EXPECT_EQ(static_cast<std::size_t>(all[i]), i);
    EXPECT_EQ(toString(all[i]), expected[i]);
    EXPECT_EQ(parseUnresolvedReason(expected[i]), all[i]);
  }
  EXPECT_FALSE(parseUnresolvedReason("unsafe"));
}

// RFC 0030 §15 item 3: the engine's incompleteness texts and their reasons.
TEST(Ledger, IncompletenessMapsToAnUnresolvedReason) {
  EXPECT_EQ(incompletenessReason("array element limit reached"),
            UnresolvedReason::Budget);
  EXPECT_EQ(incompletenessReason("call context unavailable or limit reached"),
            UnresolvedReason::Budget);
  EXPECT_EQ(incompletenessReason("array initializer exceeds element limit"),
            UnresolvedReason::Budget);
  EXPECT_EQ(incompletenessReason("function dataflow iteration limit reached"),
            UnresolvedReason::Budget);
  EXPECT_EQ(incompletenessReason("incompatible or unknown object view at call"),
            UnresolvedReason::RawCast);
  EXPECT_EQ(incompletenessReason(
                "unsupported memory copy of pointer-containing storage"),
            UnresolvedReason::RawCast);
  EXPECT_EQ(incompletenessReason("unrepresentable call context input path"),
            UnresolvedReason::Inexpressible);
  EXPECT_EQ(incompletenessReason("unsupported compound integer assignment"),
            UnresolvedReason::Unanalysed);
  EXPECT_EQ(incompletenessReason("unresolved array element selection"),
            UnresolvedReason::Unanalysed);
  EXPECT_EQ(
      incompletenessReason("array index snapshot generation is ambiguous"),
      UnresolvedReason::Unanalysed);
}

TEST(Ledger, TrustReasonsAreTheClosedListInOrder) {
  const std::vector<std::string_view> expected{
      "unsafe",          "system-api",    "library-spec", "extern-contract",
      "caller-contract", "external-unit", "concurrency"};
  const auto all = allTrustReasons();
  ASSERT_EQ(all.size(), TrustReasonCount);
  for (std::size_t i = 0; i < all.size(); ++i) {
    EXPECT_EQ(static_cast<std::size_t>(all[i]), i);
    EXPECT_EQ(toString(all[i]), expected[i]);
    EXPECT_EQ(parseTrustReason(expected[i]), all[i]);
  }
  EXPECT_FALSE(parseTrustReason("unknown-extent"));
}

TEST(Ledger, AuxiliarySpellingsRoundTrip) {
  EXPECT_EQ(toString(Certainty::Definite), "definite");
  EXPECT_EQ(toString(Certainty::Possible), "possible");
  EXPECT_EQ(parseCertainty("possible"), Certainty::Possible);
  EXPECT_EQ(toString(Verdict::Holds), "holds");
  EXPECT_EQ(toString(Verdict::Violated), "violated");
  EXPECT_EQ(toString(Verdict::Unknown), "unknown");
  EXPECT_EQ(parseVerdict("violated"), Verdict::Violated);
  EXPECT_EQ(toString(Boundary::Call), "call");
  EXPECT_EQ(parseBoundary("exit"), Boundary::Exit);
  EXPECT_EQ(toString(Linkage::Internal), "internal");
  EXPECT_EQ(parseLinkage("external"), Linkage::External);
  EXPECT_EQ(toString(ChecksMode::Verify), "verify");
  EXPECT_EQ(parseChecksMode("none"), ChecksMode::None);
  EXPECT_EQ(toString(RequireLevel::Checked), "checked");
  EXPECT_EQ(parseRequireLevel("proven"), RequireLevel::Proven);
  EXPECT_EQ(toString(LedgerScope::Program), "program");
  EXPECT_EQ(parseLedgerScope("unit"), LedgerScope::Unit);
  for (const std::string_view name :
       {"nonnull", "index", "span", "len", "disjoint", "assert", "violation"})
    EXPECT_EQ(toString(*parseCheckTemplate(name)), name);
  EXPECT_FALSE(parseCheckTemplate("strnlen"));
  EXPECT_FALSE(parseCertainty("maybe"));
}

TEST(Ledger, PhraseTemplatesMatchTheDiagnosticsSection) {
  EXPECT_EQ(phraseTemplate(UnresolvedReason::UnknownExtent),
            "the extent of '<p>' is unknown");
  EXPECT_EQ(phraseTemplate(UnresolvedReason::UnknownIndex),
            "the position of '<p>' in its object is unknown");
  EXPECT_EQ(phraseTemplate(UnresolvedReason::Inexpressible),
            "its bound has no name here");
  EXPECT_EQ(phraseTemplate(UnresolvedReason::UnknownCallee),
            "'<f>' may have freed or kept '<p>'");
  EXPECT_EQ(phraseTemplate(UnresolvedReason::Callback),
            "the target of '<slot>' is unknown");
  EXPECT_EQ(phraseTemplate(UnresolvedReason::Unanalysed),
            "WeaveC does not model this (<detail>)");
  EXPECT_EQ(phraseTemplate(UnresolvedReason::SecondOwner),
            "'<a>' and '<b>' may own the same object here");
  EXPECT_EQ(phraseTemplate(UnresolvedReason::NoZeroInit),
            "'<p>' may be uninitialised");
}

TEST(Ledger, ReasonPhrasesFillTheirPlaceholders) {
  // Every filled phrase is its template with the placeholders substituted.
  const PhraseArguments arguments{.pointer = "p",
                                  .callee = "consume",
                                  .slot = "g->frealloc",
                                  .function = "lua_newstate",
                                  .detail = "unsupported cast",
                                  .place = "g_cache",
                                  .first = "s->a",
                                  .second = "s->b"};
  for (const UnresolvedReason reason : allUnresolvedReasons()) {
    std::string expected(phraseTemplate(reason));
    const std::vector<std::pair<std::string, std::string_view>> fills{
        {"<p>", arguments.pointer},     {"<f>", arguments.callee},
        {"<slot>", arguments.slot},     {"<fn>", arguments.function},
        {"<detail>", arguments.detail}, {"<place>", arguments.place},
        {"<a>", arguments.first},       {"<b>", arguments.second}};
    for (const auto &[placeholder, value] : fills) {
      for (std::size_t at = expected.find(placeholder); at != std::string::npos;
           at = expected.find(placeholder))
        expected.replace(at, placeholder.size(), value);
    }
    EXPECT_EQ(reasonPhrase(reason, arguments), expected) << toString(reason);
  }
}

TEST(Ledger, RequireLevelMessages) {
  // RFC 0030 §4, *Require levels*.
  EXPECT_EQ(unresolvedOperationMessage(
                operationText(OperationForm::Access, "b[1000]"),
                UnresolvedReason::UnknownExtent, {.pointer = "b"}),
            "access 'b[1000]' is neither proven nor checkable: the extent of "
            "'b' is unknown [unknown-extent]");
  EXPECT_EQ(
      uncheckedOperationMessage(operationText(OperationForm::Dereference, "p"),
                                CheckTemplate::Nonnull),
      "dereference of 'p' relies on a runtime nonnull check");
  EXPECT_EQ(operationText(OperationForm::Conversion, "p", "struct s *"),
            "conversion of 'p' to 'struct s *'");
  EXPECT_EQ(operationText(OperationForm::CallTo, "memcpy"), "call to 'memcpy'");
  EXPECT_EQ(operationText(OperationForm::Release, "q"), "release of 'q'");
  EXPECT_EQ(operationText(OperationForm::BoundaryOf, "peek"),
            "boundary of 'peek'");
}

TEST(Ledger, RankOrdersOutcomes) {
  // violation > unresolved > checked > trusted > proven
  EXPECT_GT(outcomeRank(SiteOutcome::Violation),
            outcomeRank(SiteOutcome::Unresolved));
  EXPECT_GT(outcomeRank(SiteOutcome::Unresolved),
            outcomeRank(SiteOutcome::Checked));
  EXPECT_GT(outcomeRank(SiteOutcome::Checked),
            outcomeRank(SiteOutcome::Trusted));
  EXPECT_GT(outcomeRank(SiteOutcome::Trusted),
            outcomeRank(SiteOutcome::Proven));
  EXPECT_EQ(maxByRank(SiteOutcome::Trusted, SiteOutcome::Checked),
            SiteOutcome::Checked);
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
  EXPECT_FALSE((FacetDecision{.outcome = SiteOutcome::Checked,
                              .trusted = TrustReason::Unsafe}
                    .isWellFormed()));
  EXPECT_EQ(FacetDecision::checked().compact(), "checked");
  EXPECT_EQ(
      FacetDecision::unresolvedFor(UnresolvedReason::MayReleased).compact(),
      "unresolved/may-released");
  EXPECT_EQ(FacetDecision::trustedFor(TrustReason::SystemApi).compact(),
            "trusted/system-api");
}

TEST(Ledger, CompactFacetsParseStrictly) {
  EXPECT_EQ(parseCompactFacet("unresolved/unknown-extent"),
            FacetDecision::unresolvedFor(UnresolvedReason::UnknownExtent));
  EXPECT_EQ(parseCompactFacet("trusted/caller-contract"),
            FacetDecision::trustedFor(TrustReason::CallerContract));
  EXPECT_EQ(parseCompactFacet("violation"), FacetDecision::violation());
  EXPECT_FALSE(parseCompactFacet("unresolved"));
  EXPECT_FALSE(parseCompactFacet("unresolved/unsafe"));
  EXPECT_FALSE(parseCompactFacet("trusted/unknown-extent"));
  EXPECT_FALSE(parseCompactFacet("proven/unsafe"));
  EXPECT_FALSE(parseCompactFacet("proven/"));
  EXPECT_FALSE(parseCompactFacet("maybe"));
}

TEST(Ledger, RecordsMergeByRank) {
  FacetRecord record;
  EXPECT_FALSE(record.decided);
  record.decide(FacetDecision::proven());
  EXPECT_TRUE(record.decided);
  EXPECT_EQ(record.outcome(), SiteOutcome::Proven);
  record.decide(FacetDecision::trustedFor(TrustReason::Unsafe));
  EXPECT_EQ(record.outcome(), SiteOutcome::Trusted);
  record.decide(FacetDecision::checked());
  EXPECT_EQ(record.outcome(), SiteOutcome::Checked);
  record.decide(FacetDecision::proven());
  EXPECT_EQ(record.outcome(), SiteOutcome::Checked);
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

TEST(Ledger, RequirementsAreKeptPerRecord) {
  // §2.5: `char dst[16]; memcpy(dst, src, n);` with an unknown source extent
  // checks `n <= 16` for the destination, while the merged spatial facet is
  // unresolved(unknown-extent).
  FacetRecord spatial;
  spatial.addRequirement(Requirement{
      .argument = 0,
      .need = "n",
      .have = "16",
      .decision = FacetDecision::checked(),
      .check = FacetCheck{.kind = CheckTemplate::Len, .proven = false}});
  spatial.addRequirement(Requirement{
      .argument = 1,
      .need = "n",
      .have = std::nullopt,
      .decision = FacetDecision::unresolvedFor(UnresolvedReason::UnknownExtent),
      .check = std::nullopt});
  EXPECT_EQ(spatial.outcome(), SiteOutcome::Unresolved);
  EXPECT_EQ(spatial.decision.unresolved, UnresolvedReason::UnknownExtent);
  ASSERT_EQ(spatial.requirements.size(), 2U);
  EXPECT_EQ(spatial.requirements[0].decision.outcome, SiteOutcome::Checked);
  ASSERT_TRUE(spatial.requirements[0].check);
  EXPECT_EQ(spatial.requirements[0].check->kind, CheckTemplate::Len);
}

TEST(Ledger, ResetForgetsDecisionsButKeepsTheFacet) {
  Site site{.ordinal = 0, .kind = SiteKind::Deref};
  site.addFacet(Facet::Null).decide(FacetDecision::checked());
  site.addFacet(Facet::Null).check = FacetCheck{};
  FunctionLedger function{.name = "f", .sites = {site}};
  function.resetDecisions();
  const FacetRecord *null = function.sites[0].facet(Facet::Null);
  ASSERT_NE(null, nullptr);
  EXPECT_FALSE(null->decided);
  EXPECT_FALSE(null->check);
  EXPECT_FALSE(function.sites[0].hasFacet(Facet::Spatial));
}

TEST(Ledger, DefaultsOfUndecidedFacets) {
  // §2.6's table.
  EXPECT_EQ(defaultDecision(Facet::Null, false, false),
            FacetDecision::checked());
  EXPECT_EQ(defaultDecision(Facet::Null, true, false),
            FacetDecision::checked());
  EXPECT_EQ(defaultDecision(Facet::Assertion, true, false),
            FacetDecision::checked());
  EXPECT_EQ(defaultDecision(Facet::Spatial, false, true),
            FacetDecision::checked());
  EXPECT_EQ(defaultDecision(Facet::Spatial, true, true),
            FacetDecision::checked());
  EXPECT_EQ(defaultDecision(Facet::Spatial, false, false),
            FacetDecision::unresolvedFor(UnresolvedReason::Unanalysed));
  EXPECT_EQ(defaultDecision(Facet::Spatial, true, false),
            FacetDecision::unresolvedFor(UnresolvedReason::Budget));
  EXPECT_EQ(defaultDecision(Facet::Temporal, false, true),
            FacetDecision::unresolvedFor(UnresolvedReason::Unanalysed));
  EXPECT_EQ(defaultDecision(Facet::Temporal, true, true),
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
  site.addFacet(Facet::Null).decide(FacetDecision::checked());
  EXPECT_EQ(site.outcome(), SiteOutcome::Checked);
  site.addFacet(Facet::Temporal)
      .decide(FacetDecision::trustedFor(TrustReason::Concurrency));
  EXPECT_EQ(site.outcome(), SiteOutcome::Checked);
  site.addFacet(Facet::Temporal)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::MayReleased));
  EXPECT_EQ(site.outcome(), SiteOutcome::Unresolved);
}

/// Two functions whose facets cover every outcome.
static Ledger sampleLedger() {
  Ledger ledger;
  ledger.root = "/work";
  UnitLedger unit{.source = "/work/a.c", .object = "a.o", .target = "t"};
  unit.a5 = A5Counts{.nonLoweredAllocations = 2, .bypassedDeclarations = 1};

  FunctionLedger f{.name = "f", .line = 1, .linkage = Linkage::External};
  Site deref{.ordinal = 0, .kind = SiteKind::Deref, .text = "p->x"};
  deref.addFacet(Facet::Null).decide(FacetDecision::checked());
  deref.addFacet(Facet::Spatial).decide(FacetDecision::proven());
  deref.addFacet(Facet::Temporal)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::UnknownCallee));
  Site index{.ordinal = 1, .kind = SiteKind::Index, .text = "b[i]"};
  index.addFacet(Facet::Spatial).decide(FacetDecision::checked());
  Site copy{.ordinal = 2, .kind = SiteKind::LibCall, .callee = "memcpy"};
  copy.addFacet(Facet::Spatial)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::UnknownExtent));
  copy.addFacet(Facet::Null).decide(FacetDecision::proven());
  copy.addFacet(Facet::Temporal)
      .decide(FacetDecision::trustedFor(TrustReason::LibrarySpec));
  f.sites = {deref, index, copy};

  FunctionLedger g{.name = "g",
                   .line = 10,
                   .linkage = Linkage::Internal,
                   .overBudget = true};
  Site assume{.ordinal = 0, .kind = SiteKind::Assume};
  assume.addFacet(Facet::Assertion).decide(FacetDecision::checked());
  Site exit{.ordinal = 1, .kind = SiteKind::Call, .boundary = Boundary::Exit};
  exit.addFacet(Facet::Temporal)
      .decide(FacetDecision::unresolvedFor(UnresolvedReason::Budget));
  Site raw{.ordinal = 2, .kind = SiteKind::Raw};
  for (const Facet facet : {Facet::Spatial, Facet::Null, Facet::Temporal})
    raw.addFacet(facet).decide(FacetDecision::trustedFor(TrustReason::Unsafe));
  Site null{.ordinal = 3, .kind = SiteKind::Deref};
  null.addFacet(Facet::Null).decide(FacetDecision::violation());
  FacetRecord &verified = null.addFacet(Facet::Spatial);
  verified.decide(FacetDecision::proven());
  verified.check = FacetCheck{.kind = CheckTemplate::Index, .proven = true};
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

TEST(Ledger, RollupCountsSitesFacetsAndReasons) {
  const Ledger ledger = sampleLedger();
  const LedgerSummary summary = summarize(ledger);
  EXPECT_EQ(summary.sites, 7U);
  EXPECT_EQ(summary.outcomes, (OutcomeCounts{.proven = 0,
                                             .checked = 2,
                                             .violation = 1,
                                             .unresolved = 3,
                                             .trusted = 1}));
  const auto facet = [&](Facet which) {
    return summary.facets.at(static_cast<std::size_t>(which));
  };
  EXPECT_EQ(facet(Facet::Spatial), (OutcomeCounts{.proven = 2,
                                                  .checked = 1,
                                                  .violation = 0,
                                                  .unresolved = 1,
                                                  .trusted = 1}));
  EXPECT_EQ(facet(Facet::Null), (OutcomeCounts{.proven = 1,
                                               .checked = 1,
                                               .violation = 1,
                                               .unresolved = 0,
                                               .trusted = 1}));
  EXPECT_EQ(facet(Facet::Temporal), (OutcomeCounts{.proven = 1,
                                                   .checked = 0,
                                                   .violation = 0,
                                                   .unresolved = 2,
                                                   .trusted = 2}));
  EXPECT_EQ(facet(Facet::Assertion).checked, 1U);
  const auto unresolved = [&](UnresolvedReason reason) {
    return summary.unresolvedReasons.at(static_cast<std::size_t>(reason));
  };
  EXPECT_EQ(unresolved(UnresolvedReason::UnknownCallee), 1U);
  EXPECT_EQ(unresolved(UnresolvedReason::UnknownExtent), 1U);
  EXPECT_EQ(unresolved(UnresolvedReason::Budget), 1U);
  EXPECT_EQ(unresolved(UnresolvedReason::MayReleased), 0U);
  EXPECT_EQ(
      summary.trustedReasons[static_cast<std::size_t>(TrustReason::Unsafe)],
      3U);
  EXPECT_EQ(
      summary
          .trustedReasons[static_cast<std::size_t>(TrustReason::LibrarySpec)],
      1U);
  // One unresolved spatial facet out of five spatial and four null facets.
  EXPECT_DOUBLE_EQ(summary.spatialNullShare(), 1.0 / 9.0);
  EXPECT_EQ(summary.errors, 1U);
  EXPECT_EQ(summary.warnings, 1U);
  EXPECT_EQ(summary.functions, 2U);
  EXPECT_EQ(summary.overBudget, std::vector<std::string>{"g"});
  EXPECT_EQ(summary.verifyChecks, 1U);
  // §10.7: the verify check is on a spatial facet, so only that facet's
  // coverage counts it (gate G6).
  const auto verified = [&](Facet which) {
    return summary.verifyChecked.at(static_cast<std::size_t>(which));
  };
  EXPECT_EQ(verified(Facet::Spatial), 1U);
  EXPECT_EQ(verified(Facet::Null), 0U);
  EXPECT_EQ(verified(Facet::Temporal), 0U);
  EXPECT_EQ(summary.sites, summary.outcomes.total());
}

TEST(Ledger, UnitSummariesCountTheirOwnDiagnostics) {
  Ledger ledger = sampleLedger();
  ledger.units.push_back(UnitLedger{.source = "b.c"});
  // With two units, a diagnostic without a unit belongs to none of them.
  EXPECT_EQ(summarizeUnit(ledger, 0).errors, 0U);
  ledger.diagnostics[0].unit = 0;
  ledger.diagnostics[1].unit = 1;
  EXPECT_EQ(summarizeUnit(ledger, 0).errors, 1U);
  EXPECT_EQ(summarizeUnit(ledger, 0).warnings, 0U);
  EXPECT_EQ(summarizeUnit(ledger, 1).warnings, 1U);
  EXPECT_EQ(summarizeUnit(ledger, 1).sites, 0U);
  EXPECT_EQ(summarize(ledger).warnings, 1U);
  EXPECT_DOUBLE_EQ(summarizeUnit(ledger, 1).spatialNullShare(), 0.0);
}

TEST(Ledger, DerivedAssumptionCounts) {
  Ledger ledger = sampleLedger();
  ledger.scope = LedgerScope::Program;
  ledger.units[0].functions[0].sites[0].facet(Facet::Temporal)->decision =
      FacetDecision::trustedFor(TrustReason::Concurrency);
  UnitLedger other{.source = "b.c"};
  other.a5 = A5Counts{.nonLoweredAllocations = 3, .bypassedDeclarations = 0};
  ledger.units.push_back(other);
  deriveAssumptionCounts(ledger);
  ASSERT_TRUE(ledger.assumptions);
  EXPECT_EQ(ledger.assumptions->a4.concurrencySites, 1U);
  EXPECT_EQ(ledger.assumptions->a5.nonLoweredAllocations, 5U);
  EXPECT_EQ(ledger.assumptions->a5.bypassedDeclarations, 1U);
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

TEST(Ledger, FacetRowKeys) {
  EXPECT_EQ(facetRowKey(
                SiteKind::Deref, Facet::Temporal,
                FacetDecision::unresolvedFor(UnresolvedReason::UnknownCallee)),
            "deref/temporal/unresolved/unknown-callee");
  EXPECT_EQ(
      facetRowKey(SiteKind::LibCall, Facet::Spatial, FacetDecision::checked()),
      "lib-call/spatial/checked");
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
  summary.outcomes = OutcomeCounts{.proven = 3050,
                                   .checked = 980,
                                   .violation = 0,
                                   .unresolved = 150,
                                   .trusted = 30};
  summary.warnings = 2;
  return summary;
}

TEST(Ledger, UnitSummaryLine) {
  // RFC 0030 §12.4.
  EXPECT_EQ(unitSummaryLine("cJSON.c", cjsonSummary()),
            "weavec: cJSON.c: 4,210 sites: 3,050 proven, 980 checked, 150 "
            "unresolved, 30 trusted; 0 errors, 2 warnings");
  EXPECT_EQ(
      unitSummaryLine("cJSON.c", cjsonSummary(), {.checksEnforced = false}),
      "weavec: cJSON.c: 4,210 sites: 3,050 proven, 980 checkable (not "
      "enforced), 150 unresolved, 30 trusted; 0 errors, 2 warnings");
  LedgerSummary overBudget = cjsonSummary();
  overBudget.overBudget = {"cJSON_ParseWithLengthOpts"};
  EXPECT_EQ(unitSummaryLine("cJSON.c", overBudget),
            "weavec: cJSON.c: 4,210 sites: 3,050 proven, 980 checked, 150 "
            "unresolved, 30 trusted; 0 errors, 2 warnings; 1 function over "
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
            "weavec: a.c: 1 site: 0 proven, 0 checked, 1 violation, 0 "
            "unresolved, 0 trusted; 1 error, 1 warning");
}

TEST(Ledger, ProgramSummaryLine) {
  LedgerSummary summary;
  summary.sites = 9876;
  summary.outcomes = OutcomeCounts{.proven = 9000,
                                   .checked = 800,
                                   .violation = 0,
                                   .unresolved = 70,
                                   .trusted = 6};
  summary.warnings = 1;
  const std::vector<std::string> inputs{"libz.a"};
  EXPECT_EQ(programSummaryLine({.program = "minigzip",
                                .units = 3,
                                .inputsWithoutRecords = inputs,
                                .unverifiedRequirements = 12,
                                .unverifiedInvariants = 0},
                               summary),
            "weavec: program minigzip: 9,876 sites in 3 units: 9,000 proven, "
            "800 checked, 70 unresolved, 6 trusted; 0 errors, 1 warning; 1 "
            "input without a WeaveC record (libz.a); unverified: 12 exported "
            "requirements (A1), 0 header invariants (A3)");
  EXPECT_EQ(programSummaryLine({.program = "p",
                                .units = 1,
                                .inputsWithoutRecords = {},
                                .unverifiedRequirements = 1,
                                .unverifiedInvariants = 1},
                               LedgerSummary{}),
            "weavec: program p: 0 sites in 1 unit: 0 proven, 0 checked, 0 "
            "unresolved, 0 trusted; 0 errors, 0 warnings; unverified: 1 "
            "exported requirement (A1), 1 header invariant (A3)");
}

TEST(Ledger, SummaryLineOfALedgerFollowsItsScope) {
  Ledger ledger = sampleLedger();
  EXPECT_EQ(summaryLine(ledger, "a.c"),
            "weavec: a.c: 7 sites: 0 proven, 2 checked, 1 violation, 3 "
            "unresolved, 1 trusted; 1 error, 1 warning; 1 function over "
            "budget (g)");
  ledger.scope = LedgerScope::Program;
  ledger.assumptions = Assumptions{};
  ledger.assumptions->a1.exportedRequirements = 14;
  ledger.assumptions->a1.verified = 11;
  ledger.assumptions->a3.unverified = 2;
  ledger.assumptions->a3.inputsWithoutRecords = {"liblua.a", "libm.a"};
  EXPECT_EQ(summaryLine(ledger, "lua"),
            "weavec: program lua: 7 sites in 1 unit: 0 proven, 2 checked, 1 "
            "violation, 3 unresolved, 1 trusted; 1 error, 1 warning; 1 "
            "function over budget (g); 2 inputs without a WeaveC record "
            "(liblua.a, libm.a); unverified: 3 exported requirements (A1), 2 "
            "header invariants (A3)");
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
