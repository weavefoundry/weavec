//===- AnalysisSummaryTest.cpp - Tests for the analysis's summary lines ---===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Frontend/AnalysisSummary.h"

#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace weavec::frontend {

using core::Facet;
using core::FacetDecision;

/// One unit with a dereference whose spatial facet is not proven, as a unit
/// pipeline leaves it.
static core::Ledger unitLedger() {
  core::Ledger ledger;
  core::FunctionLedger function{.name = "f"};
  core::Site site{.ordinal = 0,
                  .kind = core::SiteKind::Deref,
                  .location = {.file = "", .line = 2, .column = 3, .opaque = 0},
                  .text = "*p"};
  site.addFacet(Facet::Spatial)
      .decide(
          FacetDecision::unresolvedFor(core::UnresolvedReason::UnknownExtent));
  function.sites.push_back(site);
  core::Site proven{.ordinal = 1,
                    .kind = core::SiteKind::Deref,
                    .location = {.file = "", .line = 3, .column = 3},
                    .text = "*q"};
  proven.addFacet(Facet::Null).decide(FacetDecision::proven());
  function.sites.push_back(proven);
  ledger.units.emplace_back();
  ledger.units.front().functions.push_back(function);
  return ledger;
}

static core::LedgerDiagnostic diagnostic(std::string_view id,
                                         core::Severity severity,
                                         core::Certainty certainty,
                                         std::uint32_t line) {
  return core::LedgerDiagnostic{
      .id = std::string(id),
      .severity = severity,
      .certainty = certainty,
      .message = "m",
      .location = {.file = "a.c", .line = line, .column = 1, .opaque = 0}};
}

TEST(AnalysisSummary, TheUnitLineCountsSitesAndFindings) {
  core::Ledger ledger = unitLedger();
  ledger.diagnostics = {diagnostic(core::diag::Leak, core::Severity::Warning,
                                   core::Certainty::Definite, 1)};
  std::string line;
  llvm::raw_string_ostream os(line);
  printUnitSummary(ledger, "a.c", DiagnosticControl{}, os);
  EXPECT_EQ(line, "weavec: a.c: 2 sites: 1 proven, 1 not proven, 0 "
                  "violations, 0 trusted; 0 errors, 1 warning\n");
}

TEST(AnalysisSummary, TheProgramLineNamesItsUnits) {
  core::Ledger ledger = unitLedger();
  ledger.units.push_back(ledger.units.front());
  ledger.units.back().functions.front().overBudget = true;
  ledger.units.back().functions.front().name = "g";
  std::string line;
  llvm::raw_string_ostream os(line);
  printProgramSummary(ledger, "prog", DiagnosticControl{}, os);
  EXPECT_EQ(line, "weavec: program prog: 4 sites in 2 units: 2 proven, 2 not "
                  "proven, 0 violations, 0 trusted; 0 errors, 0 warnings; 1 "
                  "function over budget (g)\n");
}

TEST(AnalysisSummary, AppliesTheWarningFlagsToTheDiagnostics) {
  core::Ledger ledger = unitLedger();
  ledger.diagnostics = {
      diagnostic(core::diag::UseAfterFree, core::Severity::Warning,
                 core::Certainty::Possible, 1),
      diagnostic(core::diag::UseAfterFree, core::Severity::Error,
                 core::Certainty::Definite, 2),
      diagnostic(core::diag::Leak, core::Severity::Warning,
                 core::Certainty::Definite, 3),
  };
  core::Site &site = ledger.units.front().functions.front().sites.front();
  core::FacetRecord &temporal = site.addFacet(Facet::Temporal);
  temporal.decide(
      FacetDecision::unresolvedFor(core::UnresolvedReason::MayReleased));
  temporal.diagnostic = 0;
  core::FacetRecord &spatial = *site.facet(Facet::Spatial);
  spatial.diagnostic = 1;

  DiagnosticControl control;
  std::string error;
  ASSERT_TRUE(control.parse("-Wno-weavec-use-after-free", error));
  ASSERT_TRUE(control.parse("-Werror=weavec-leak", error));
  applyDiagnosticControl(ledger, control);
  ASSERT_EQ(ledger.diagnostics.size(), 2U);
  EXPECT_EQ(ledger.diagnostics[0].certainty, core::Certainty::Definite);
  EXPECT_EQ(ledger.diagnostics[0].severity, core::Severity::Error);
  EXPECT_EQ(ledger.diagnostics[1].id, core::diag::Leak);
  EXPECT_EQ(ledger.diagnostics[1].severity, core::Severity::Error);
  // The dropped diagnostic's facet loses its link; the kept one follows.
  EXPECT_EQ(temporal.diagnostic, std::nullopt);
  EXPECT_EQ(spatial.diagnostic, 0U);
}

TEST(AnalysisSummary, SummaryNamesAreRelativeToTheJob) {
  EXPECT_EQ(summaryName("src/a.c", "/proj"), "src/a.c");
  EXPECT_EQ(summaryName("../a.c", "/proj/build"), "../a.c");
  EXPECT_EQ(summaryName("/proj/src/a.c", "/proj"), "src/a.c");
  EXPECT_EQ(summaryName("/proj/src/../a.c", "/proj"), "a.c");
  EXPECT_EQ(summaryName("/elsewhere/a.c", "/proj"), "/elsewhere/a.c");
}

} // namespace weavec::frontend
