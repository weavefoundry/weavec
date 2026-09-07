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
  const auto *summary = result.summary("limited");
  ASSERT_NE(summary, nullptr);
  EXPECT_FALSE(summary->requiresExtent.contains(0));
  EXPECT_TRUE(
      summary->incomplete.contains("unsupported extent requirement condition"));
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
  const auto *summary = result.summary("limited");
  ASSERT_NE(summary, nullptr);
  EXPECT_FALSE(summary->requiresExtent.contains(0));
  EXPECT_TRUE(
      summary->incomplete.contains("unsupported extent requirement condition"));
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
  const auto *summary = result.summary("limited");
  ASSERT_NE(summary, nullptr);
  EXPECT_FALSE(summary->requiresExtent.contains(0));
  EXPECT_TRUE(
      summary->incomplete.contains("unsupported extent requirement condition"));
}

TEST(GuardCompleteness, NumericReturnsAndStoresLoseIncompleteProjections) {
  const auto result = analyze(R"c(
    unsigned choose(unsigned a, unsigned b, unsigned c, unsigned d,
                    unsigned e, unsigned f, unsigned g, unsigned h,
                    unsigned n, unsigned m) {
      if (!a || !b || !c || !d || !e || !f || !g || !h) return 0;
      if (n < m) return 1;
      return 2;
    }
    void output(unsigned *out, unsigned a, unsigned b, unsigned c, unsigned d,
                unsigned e, unsigned f, unsigned g, unsigned h,
                unsigned n, unsigned m) {
      if (!a || !b || !c || !d || !e || !f || !g || !h) return;
      if (n < m) { *out = 1; return; }
      *out = 2;
    }
  )c");
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"choose", "output"}) {
    SCOPED_TRACE(name);
    const auto *summary = result.summary(name);
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(
        summary->incomplete.contains("unsupported numeric output projection"));
    const auto path = std::string_view(name) == "choose"
                          ? core::SummaryPath::result()
                          : core::SummaryPath::param(0).deref();
    const auto found = summary->numericOutputs.find(path);
    ASSERT_NE(found, summary->numericOutputs.end());
    EXPECT_TRUE(std::ranges::any_of(
        found->second, [](const auto &value) { return !value.value; }));
  }
}

TEST(GuardCompleteness, RetainedScalarFactsCanImplyANumericPredicate) {
  const auto result = analyze(R"c(
    void fits(char *p, unsigned a, unsigned b, unsigned c, unsigned d,
              unsigned e, unsigned f, unsigned g, unsigned n) {
      if (!a || !b || !c || !d || !e || !f || !g) return;
      if (n == 3) p[n] = 0;
    }
    void good(void) {
      char four[4]; fits(four, 1,1,1,1,1,1,1, 3);
    }
    void bad(void) {
      char three[3]; fits(three, 1,1,1,1,1,1,1, 3);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
  const auto *summary = result.summary("fits");
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(summary->requiresExtent.contains(0));
  EXPECT_FALSE(
      summary->incomplete.contains("unsupported extent requirement condition"));
}

TEST(GuardCompleteness, CanonicalLoopBoundaryCanExcludeItsIndexPredicate) {
  const auto result = analyze(R"c(
    void fill(char *p, unsigned n, unsigned cap) {
      for (unsigned i = 0; i < n && i < cap; ++i) p[i] = 0;
    }
    void good(void) { char two[2]; fill(two, 10, 2); }
    void bad(void) { char two[2]; fill(two, 3, 3); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
  const auto *summary = result.summary("fill");
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(summary->requiresExtent.contains(0));
  EXPECT_FALSE(
      summary->incomplete.contains("unsupported extent requirement condition"));
}
