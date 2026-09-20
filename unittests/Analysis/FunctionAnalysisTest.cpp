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

using Strings = std::vector<std::string>;

TEST(FunctionAnalyzer, CleanCodeProducesNoDiagnostics) {
  const auto result = analyze(R"c(
    void f(void) {
      int *p = malloc(sizeof(int));
      if (!p) return;
      *p = 1;
      use(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
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
  EXPECT_EQ(
      messages(result.diagnostics),
      std::vector<std::string>{"8: use of 'p' after it may have been freed"});
}

TEST(FunctionAnalyzer, UnsafeFunctionReportsTemporalViolations) {
  // RFC 0030 §6.1: temporal state is tracked inside an unsafe region exactly
  // as outside it, and a definite violation is still an error.
  const auto result = analyze(R"c(
    __attribute__((annotate("weavec.unsafe")))
    void f(int *p) {
      free(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{"5: 'p' is freed twice"});
  EXPECT_EQ(result.diagnostics.diagnostics()[0].severity,
            core::Severity::Error);
}

TEST(FunctionAnalyzer, UnsafeBlockReportsTemporalViolations) {
  const auto result = analyze(R"c(
    void f(int *p) {
      free(p);
      __attribute__((annotate("weavec.unsafe"))) {
        use(p);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            Strings{"5: use of 'p' after it was freed"});
}

TEST(FunctionAnalyzer, UnsafeBlockEffectsEscape) {
  // RFC 0004, "Unsafe regions": the block is analysed, so a free inside it
  // is checked against the uses after it.
  const auto result = analyze(R"c(
    void f(int *p) {
      __attribute__((annotate("weavec.unsafe"))) {
        free(p);
      }
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            Strings{"6: use of 'p' after it was freed"});
}

TEST(FunctionAnalyzer, UnsafeRegionsDropNoDiagnostic) {
  // RFC 0030 §6.1: the `inUnsafe` suppression is gone; a possible finding
  // inside a region is reported as outside it, with "may" wording.
  const auto result = analyze(R"c(
    void f(int *p, int c) {
      if (c) free(p);
      __attribute__((annotate("weavec.unsafe"))) {
        use(p);
      }
    }
    __attribute__((annotate("weavec.unsafe"))) void g(int *p) {
      free(p);
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: use of 'p' after it may have been freed",
                     "10: use of 'p' after it was freed"}));
  EXPECT_EQ(result.diagnostics.diagnostics()[0].severity,
            core::Severity::Warning);
  EXPECT_EQ(result.diagnostics.diagnostics()[1].severity,
            core::Severity::Error);
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

TEST(FunctionAnalyzer, UnannotatedParametersAreNotReported) {
  // RFC 0030 removed `--report-unannotated` and `annotation-required`: an
  // unannotated interface is reported by nothing.
  const auto result = analyze(R"c(
    void f(int *p, int n, int *__attribute__((annotate("weavec.borrowed"))) q) {}
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
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
