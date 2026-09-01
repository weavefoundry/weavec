//===- FunctionAnalysisTest.cpp - Tests for the local ownership checker ---===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/FunctionAnalysis.h"

#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using weavec::test::analyze;
using weavec::test::ids;

TEST(LocalOwnershipChecker, CleanCodeProducesNoDiagnostics) {
  const auto result = analyze(R"c(
    void f(void) {
      int *p = malloc(sizeof(int));
      *p = 1;
      use(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

TEST(LocalOwnershipChecker, DetectsUseAfterFree) {
  const auto result = analyze(R"c(
    void f(void) {
      int *p = malloc(sizeof(int));
      free(p);
      *p = 2;
    }
  )c");
  ASSERT_TRUE(result.ast);
  ASSERT_EQ(result.diagnostics.size(), 1U);
  const core::Diagnostic &d = result.diagnostics.diagnostics()[0];
  EXPECT_EQ(d.id, core::diag::UseAfterFree);
  EXPECT_EQ(d.severity, core::Severity::Error);
  EXPECT_NE(d.message.find("'p'"), std::string::npos);
  ASSERT_EQ(d.notes.size(), 1U);
  EXPECT_LT(d.notes[0].location.line, d.location.line)
      << "the note points at the earlier free";
}

TEST(LocalOwnershipChecker, DetectsDoubleFree) {
  const auto result = analyze(R"c(
    void f(int *p) {
      free(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            std::vector<std::string>{std::string(core::diag::DoubleFree)});
}

TEST(LocalOwnershipChecker, ReassignmentReinitializes) {
  const auto result = analyze(R"c(
    void f(void) {
      int *p = malloc(4);
      free(p);
      p = malloc(8);
      use(p);
      free(p);
      p = NULL;
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

TEST(LocalOwnershipChecker, BranchesAreJoinedConservatively) {
  const auto result = analyze(R"c(
    void f(int c) {
      int *p = malloc(4);
      if (c)
        free(p);
      else
        use(p);   /* fine: p is live on this path */
      use(p);     /* may be freed */
    }
  )c");
  ASSERT_TRUE(result.ast);
  ASSERT_EQ(result.diagnostics.size(), 1U);
  EXPECT_EQ(result.diagnostics.diagnostics()[0].id, core::diag::UseAfterFree);
}

TEST(LocalOwnershipChecker, UnsafeFunctionIsSkipped) {
  const auto result = analyze(R"c(
    __attribute__((annotate("weavec.unsafe")))
    void f(int *p) {
      free(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

TEST(LocalOwnershipChecker, UnsafeBlockIsSkipped) {
  const auto result = analyze(R"c(
    void f(int *p) {
      free(p);
      __attribute__((annotate("weavec.unsafe"))) {
        use(p);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

TEST(FunctionAnalyzer, ReportsInvalidAnnotation) {
  const auto result = analyze(R"c(
    __attribute__((annotate("weavec.nonsense")))
    void f(void) {}
  )c");
  ASSERT_TRUE(result.ast);
  ASSERT_EQ(result.diagnostics.size(), 1U);
  EXPECT_EQ(result.diagnostics.diagnostics()[0].id,
            core::diag::InvalidAnnotation);
  EXPECT_EQ(result.diagnostics.diagnostics()[0].severity,
            core::Severity::Warning);
}

TEST(FunctionAnalyzer, ReportUnannotatedIsOptIn) {
  const char *code = R"c(
    void f(int *p, int n, int *__attribute__((annotate("weavec.borrowed"))) q) {}
  )c";

  const auto quiet = analyze(code);
  ASSERT_TRUE(quiet.ast);
  EXPECT_TRUE(quiet.diagnostics.empty());

  AnalysisOptions options;
  options.reportUnannotated = true;
  const auto loud = analyze(code, options);
  ASSERT_TRUE(loud.ast);
  ASSERT_EQ(loud.diagnostics.size(), 1U) << "only the unannotated pointer";
  EXPECT_EQ(loud.diagnostics.diagnostics()[0].id,
            core::diag::AnnotationRequired);
  EXPECT_NE(loud.diagnostics.diagnostics()[0].message.find("'p'"),
            std::string::npos);
}

} // namespace
} // namespace weavec::analysis
