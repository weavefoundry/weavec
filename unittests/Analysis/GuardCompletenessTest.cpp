//===- GuardCompletenessTest.cpp - Complete numeric guards (RFC 0017) -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include <algorithm>
#include <string_view>

#include "gtest/gtest.h"

using namespace weavec;
using namespace weavec::test;

static unsigned countId(const AnalysisResult &result, std::string_view id) {
  return static_cast<unsigned>(
      std::ranges::count_if(result.diagnostics.diagnostics(),
                            [id](const core::Diagnostic &diagnostic) {
                              return diagnostic.id == id;
                            }));
}

/// The spatial facet of the site spelled `text` at `line`, as its outcome,
/// with the reason when it is unresolved (`unresolved/unknown-extent`).
static std::string spatialAt(const AnalysisResult &result, unsigned line,
                             std::string_view text) {
  for (const core::UnitLedger &unit : result.planned.ledger.units)
    for (const core::FunctionLedger &function : unit.functions)
      for (const core::Site &site : function.sites)
        if (site.location.line == line && site.text == text)
          if (const core::FacetRecord *record =
                  site.facet(core::Facet::Spatial)) {
            std::string out(core::toString(record->outcome()));
            if (record->outcome() == core::SiteOutcome::Unresolved)
              out += "/" + std::string(record->decision.reasonText());
            return out;
          }
  return "none";
}

TEST(GuardCompleteness, CapacityCannotDropARelationalRequirementPremise) {
  const auto result = analyze(R"c(
    void limited(char *p, unsigned a, unsigned b, unsigned c, unsigned d,
                 unsigned e, unsigned f, unsigned g, unsigned h,
                 unsigned n, unsigned m) {
      if (!a || !b || !c || !d || !e || !f || !g || !h) return;
      if (n < m) p[n] = 0;
    }
    void good(void) {
      char two[2]; limited(two, 1,1,1,1,1,1,1,1, 2,2);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(GuardCompleteness, OmittedScalarFactsCannotProveAnOmittedPredicate) {
  const auto result = analyze(R"c(
    void limited(char *p, unsigned a, unsigned b, unsigned c, unsigned d,
                 unsigned e, unsigned f, unsigned g, unsigned h, unsigned n) {
      if (!a || !b || !c || !d || !e || !f || !g || !h) return;
      if (n == 3) p[n] = 0;
    }
    void good(void) {
      char two[2]; limited(two, 1,1,1,1,1,1,1,1, 2);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(GuardCompleteness, UnknownCallConditionCannotDisappearAtCapacity) {
  const auto result = analyze(R"c(
    void limited(char *p, unsigned a, unsigned b, unsigned c, unsigned d,
                 unsigned e, unsigned f, unsigned g, unsigned h, unsigned n) {
      if (!a || !b || !c || !d || !e || !f || !g || !h) return;
      if (cond()) p[n] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
}

// A guarded access is none of the §7.5 rules, and RFC 0031 §6.1's
// summaries carry no extent requirement: the access stays unresolved in
// `fits`'s own unit, never proven. At link the context of `bad`'s call
// stores past `three` (RFC 0031 *Implementation amendments*, "Stores past
// the caller's object"), and `good` gets no finding.
TEST(GuardCompleteness, RetainedScalarFactsCanImplyANumericPredicate) {
  static constexpr const char *Callee = R"c(
    void fits(char *p, unsigned a, unsigned b, unsigned c, unsigned d,
              unsigned e, unsigned f, unsigned g, unsigned n) {
      if (!a || !b || !c || !d || !e || !f || !g) return;
      if (n == 3) p[n] = 0;
    }
  )c";
  const auto result = analyze(Callee);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(spatialAt(result, 5, "p[n]"), "unresolved/unknown-extent");
  const auto linked = analyzeAtLink(Callee, R"c(
    void fits(char *p, unsigned a, unsigned b, unsigned c, unsigned d,
              unsigned e, unsigned f, unsigned g, unsigned n);
    void good(void) {
      char four[4]; fits(four, 1,1,1,1,1,1,1, 3);
    }
    void bad(void) {
      char three[3]; fits(three, 1,1,1,1,1,1,1, 3);
    }
  )c");
  EXPECT_EQ(countId(linked, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(linked.diagnostics));
}

// RFC 0031 §6.1: `fill`'s minimum is exported as no requirement; the access
// stays unresolved in `fill`'s own unit, never proven. At link the context
// of `bad`'s call stores past `two` (RFC 0031 *Implementation amendments*,
// "Stores past the caller's object"), and `good` gets no finding.
TEST(GuardCompleteness, CanonicalLoopBoundaryCanExcludeItsIndexPredicate) {
  static constexpr const char *Callee = R"c(
    void fill(char *p, unsigned n, unsigned cap) {
      for (unsigned i = 0; i < n && i < cap; ++i) p[i] = 0;
    }
  )c";
  const auto result = analyze(Callee);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(spatialAt(result, 3, "p[i]"), "unresolved/unknown-extent");
  const auto linked = analyzeAtLink(Callee, R"c(
    void fill(char *p, unsigned n, unsigned cap);
    void good(void) { char two[2]; fill(two, 10, 2); }
    void bad(void) { char two[2]; fill(two, 3, 3); }
  )c");
  EXPECT_EQ(countId(linked, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(linked.diagnostics));
}
