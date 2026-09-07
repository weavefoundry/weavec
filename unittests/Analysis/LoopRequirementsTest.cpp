//===- LoopRequirementsTest.cpp - Loop requirements (RFC 0017) ------------===//
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

static void expectIncompleteLoop(const AnalysisResult &result,
                                 const char *name) {
  SCOPED_TRACE(name);
  const auto *summary = result.summary(name);
  ASSERT_NE(summary, nullptr);
  EXPECT_FALSE(summary->requiresExtent.contains(0));
  EXPECT_TRUE(summary->incomplete.contains(
      "unsupported extent requirement projection"));
}

TEST(LoopRequirements, EarlyExitsDoNotRequireAnUnreachedMaximum) {
  const auto result = analyze(R"c(
    void before(char *p, unsigned n) {
      for (unsigned i = 0; i < n; i++) {
        if (i == 2) break;
        p[i] = 0;
      }
    }
    void after(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) {
        p[i] = 0;
        if (i == 2) break;
      }
    }
    void returns(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) {
        if (i == 2) return;
        p[i] = 0;
      }
    }
    void jumps(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) {
        if (i == 2) goto done;
        p[i] = 0;
      }
    done:;
    }
    void skips(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) {
        if (i >= 2) continue;
        p[i] = 0;
      }
    }
    void callers(void) {
      char two[2], three[3];
      before(two, 10); after(three, 10); returns(two, 10);
      jumps(two, 10); skips(two, 10);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U)
      << ::testing::PrintToString(messages(result.diagnostics));
  for (const auto *name : {"before", "after", "returns", "jumps", "skips"})
    expectIncompleteLoop(result, name);
}

TEST(LoopRequirements, NoncanonicalInitializationAndStrideAreIncomplete) {
  const auto result = analyze(R"c(
    void nonzero(char *p, unsigned n) {
      for (unsigned i = 8; i < n; ++i) p[i] = 0;
    }
    void stride(char *p, unsigned n) {
      for (unsigned i = 0; i < n; i += 3) p[i] = 0;
    }
    void decrement(char *p, unsigned n) {
      for (unsigned i = 0; i < n; --i) p[i] = 0;
    }
    void missing_init(char *p, unsigned n) {
      unsigned i = 0;
      for (; i < n; ++i) p[i] = 0;
    }
    void callers(void) {
      char one[1]; nonzero(one, 2); stride(one, 3); decrement(one, 2);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U)
      << ::testing::PrintToString(messages(result.diagnostics));
  for (const auto *name : {"nonzero", "stride", "decrement", "missing_init"})
    expectIncompleteLoop(result, name);
}

TEST(LoopRequirements, MutatedAndEscapedControlsAreIncomplete) {
  const auto result = analyze(R"c(
    void mutate_index(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) { p[i] = 0; i += 2; }
    }
    void mutate_bound(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) { p[i] = 0; n = 1; }
    }
    void escape_index(char *p, unsigned n) {
      unsigned i, *cursor = &i;
      for (i = 0; i < n; ++i) { p[i] = 0; *cursor += 2; }
    }
    void escape_bound(char *p, unsigned n) {
      unsigned *count = &n;
      for (unsigned i = 0; i < n; ++i) { p[i] = 0; *count = 1; }
    }
    void callers(void) {
      char one[1]; mutate_index(one, 2); mutate_bound(one, 10);
      escape_index(one, 2); escape_bound(one, 10);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U)
      << ::testing::PrintToString(messages(result.diagnostics));
  for (const auto *name :
       {"mutate_index", "mutate_bound", "escape_index", "escape_bound"})
    expectIncompleteLoop(result, name);
}

TEST(LoopRequirements, UnsupportedBodyControlDoesNotAcquireAMustRequirement) {
  const auto result = analyze(R"c(
    void conditional(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) if (i < 2) p[i] = 0;
    }
    void nested(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i)
        while (i < 2) { p[i] = 0; break; }
    }
    void call(char *p, unsigned n) {
      for (unsigned i = 0; i < n; ++i) { cond(); p[i] = 0; }
    }
    void reused(char *p, unsigned n) {
      unsigned i;
      for (i = 0; i < n; ++i) p[i] = 0;
      for (i = 0; i < n; i += 2) p[i] = 0;
    }
    void outside(char *p, unsigned n) {
      unsigned i;
      for (i = 0; i < n; ++i) {}
      p[i] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  for (const auto *name :
       {"conditional", "nested", "call", "reused", "outside"})
    expectIncompleteLoop(result, name);
}

TEST(LoopRequirements, IncrementMustFitTheIndexStorageType) {
  const auto result = analyze(R"c(
    void narrow(char *p, unsigned n) {
      for (unsigned char i = 0; i < n; ++i) p[i] = 0;
    }
    void signed_narrow(char *p, unsigned n) {
      for (signed char i = 0; i < n; ++i) p[i] = 0;
    }
    void inclusive(char *p, unsigned n) {
      for (unsigned i = 0; i <= n; ++i) p[i] = 0;
    }
    void truncated(char *p, unsigned n) {
      for (unsigned i = 0; (unsigned char)i < n; ++i) p[i] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  for (const auto *name : {"narrow", "signed_narrow", "inclusive", "truncated"})
    expectIncompleteLoop(result, name);
}

TEST(LoopRequirements, CanonicalMinimaStillComposeThroughWrappers) {
  const auto result = analyze(R"c(
    void fill(char *p, unsigned n, unsigned cap) {
      for (unsigned i = 0; i < n && i < cap; ++i) p[i] = 0;
    }
    void wrapper(char *p, unsigned n, unsigned cap) { fill(p, n, cap); }
    void explicit_min(char *p, unsigned n, unsigned cap) {
      for (unsigned i = 0; i < (n < cap ? n : cap); i++) p[i] = 0;
    }
    void constant_min(char *p, unsigned n) {
      for (unsigned i = 0; i < n && i < 2; i++) p[i] = 0;
    }
    void good(void) {
      char two[2]; wrapper(two, 10, 2); wrapper(two, 2, 10);
      explicit_min(two, 10, 2); constant_min(two, 10);
    }
    void bad(void) { char two[2]; wrapper(two, 3, 3); explicit_min(two, 3, 3); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U)
      << ::testing::PrintToString(messages(result.diagnostics));
  for (const auto *name : {"fill", "wrapper", "explicit_min", "constant_min"}) {
    SCOPED_TRACE(name);
    const auto *summary = result.summary(name);
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->requiresExtent.contains(0));
    EXPECT_FALSE(summary->incomplete.contains(
        "unsupported extent requirement projection"));
  }
}

TEST(LoopRequirements, CanonicalDeclarationsAndAssignmentsKeepTheirBounds) {
  const auto result = analyze(R"c(
    void signed_index(char *p, int n) {
      for (int i = 0; i < n; i++) p[i] = 0;
    }
    void assigned_index(char *p, unsigned n) {
      unsigned i;
      for (i = 0; i < n; ++i) p[i] = 0;
    }
    void inclusive_constant(char *p) {
      for (unsigned i = 0; i <= 8; ++i) p[i] = 0;
    }
    void good(void) {
      char two[2], nine[9]; signed_index(two, 2); assigned_index(two, 2);
      inclusive_constant(nine);
    }
    void bad(void) {
      char two[2], eight[8]; signed_index(two, 3); assigned_index(two, 3);
      inclusive_constant(eight);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 3U)
      << ::testing::PrintToString(messages(result.diagnostics));
  for (const auto *name :
       {"signed_index", "assigned_index", "inclusive_constant"}) {
    SCOPED_TRACE(name);
    const auto *summary = result.summary(name);
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->requiresExtent.contains(0));
  }
}
