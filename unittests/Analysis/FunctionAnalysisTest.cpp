//===- FunctionAnalysisTest.cpp - Tests for the per-function driver -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Smoke tests for `FunctionAnalyzer`: the basic detections, the unsafe
// escape hatch and the annotation checks. The dataflow itself is exercised
// in DataflowTest.cpp.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/FunctionAnalysis.h"

#include "TestUtils.h"

#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using weavec::test::analyze;
using weavec::test::ids;
using weavec::test::messages;
using weavec::test::notes;

TEST(FunctionAnalyzer, CleanCodeProducesNoDiagnostics) {
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

TEST(FunctionAnalyzer, DetectsUseAfterFree) {
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
  EXPECT_EQ(d.message, "use of 'p' after it was freed");
  EXPECT_EQ(d.location.line, 5U);
  ASSERT_EQ(d.notes.size(), 1U);
  EXPECT_EQ(d.notes[0].message, "freed here");
  EXPECT_EQ(d.notes[0].location.line, 4U);
}

TEST(FunctionAnalyzer, DetectsDoubleFree) {
  const auto result = analyze(R"c(
    void f(int *p) {
      free(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            std::vector<std::string>{std::string(core::diag::DoubleFree)});
  EXPECT_EQ(messages(result.diagnostics),
            std::vector<std::string>{"4: 'p' is freed twice"});
  EXPECT_EQ(notes(result.diagnostics),
            std::vector<std::string>{"previously freed here"});
}

TEST(FunctionAnalyzer, ReassignmentReinitializes) {
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

TEST(FunctionAnalyzer, BranchesAreJoinedConservatively) {
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
  EXPECT_EQ(messages(result.diagnostics),
            std::vector<std::string>{"8: use of 'p' after it was freed"});
}

TEST(FunctionAnalyzer, UnsafeFunctionIsSkipped) {
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

TEST(FunctionAnalyzer, UnsafeBlockIsSkipped) {
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

TEST(FunctionAnalyzer, UnsafeBlockIsIdentityTransfer) {
  // RFC 0002, "Unsafe interaction": the block contributes no facts, so a
  // free inside it is invisible outside. The escape rule is a later RFC.
  const auto result = analyze(R"c(
    void f(int *p) {
      __attribute__((annotate("weavec.unsafe"))) {
        free(p);
      }
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

TEST(FunctionAnalyzer, DeclarationsAndBodylessFunctionsAreIgnored) {
  const auto result = analyze(R"c(
    void g(int *p);
    static inline void h(int *p) __attribute__((annotate("weavec.unsafe")));
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

TEST(FunctionAnalyzer, DumpStreamDescribesEveryFunction) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  AnalysisOptions options;
  options.dumpStream = &stream;
  const auto result = analyze(R"c(
    struct s { int *buf; };
    void f(struct s *p, int c) {
      int x = 0;
      int *a = &x;
      if (c) free(p->buf);
      use(a);
    }
    void g(void) {}
  )c",
                              options);
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_NE(dump.find("function 'f':"), std::string::npos) << dump;
  EXPECT_NE(dump.find("function 'g':"), std::string::npos) << dump;
  EXPECT_NE(dump.find("p (param, unknown)"), std::string::npos) << dump;
  EXPECT_NE(dump.find("a (local, mutable)"), std::string::npos) << dump;
  EXPECT_NE(dump.find("moved{p->buf@"), std::string::npos) << dump;
}

} // namespace
} // namespace weavec::analysis
