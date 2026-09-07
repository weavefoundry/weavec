//===- NumericInputsTest.cpp - Call-entry sizes (RFC 0017) ----------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include "llvm/Support/raw_ostream.h"

#include "gtest/gtest.h"

using namespace weavec;
using namespace weavec::test;

static unsigned countId(const AnalysisResult &result, std::string_view id) {
  return static_cast<unsigned>(std::ranges::count(ids(result.diagnostics), id));
}

TEST(NumericInputs, AddressTakingPreservesTheNumericValue) {
  const auto result = analyze(R"c(
    void bad(void) {
      unsigned n = 4;
      unsigned *alias = &n;
      char p[4];
      p[n] = 0;
      (void)alias;
    }
    void good(void) {
      unsigned n = 0;
      unsigned *alias = &n;
      char p[4];
      p[n] = 0;
      (void)alias;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(NumericInputs, UnknownWritesStillInvalidateAddressTakenValues) {
  const auto result = analyze(R"c(
    void overwrite(unsigned *n);
    void bad(void) {
      unsigned n = 0;
      overwrite(&n);
      int *p = malloc(4);
      if (!p) return;
      free(p);
      if (n == 1) *p = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(NumericInputs, ReallocatedFieldUsesTheCountBeforeTheCallWritesIt) {
  const auto result = analyze(R"c(
    struct vec { int *items; int cap; };
    int grow(struct vec *v) {
      int *p = realloc(v->items, sizeof *p * (v->cap + 8));
      if (!p) return 0;
      v->items = p;
      v->cap += 8;
      return 1;
    }
    void bad(void) {
      struct vec v = {malloc(8 * sizeof(int)), 8};
      if (!grow(&v)) { free(v.items); return; }
      v.items[15] = 0;
      v.items[16] = 0;
      free(v.items);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(NumericInputs, ReturnedProductUsesTheInputBeforeAnOutputWrite) {
  const auto result = analyze(R"c(
    char *make(unsigned *n) {
      char *p = malloc(*n * 2u);
      *n += 1;
      return p;
    }
    void bad(void) {
      unsigned n = 4;
      char *p = make(&n);
      if (!p) return;
      p[7] = 0;
      p[8] = 0;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(NumericInputs, NumericOutputsAreComputedFromTheSameEntryState) {
  const auto result = analyze(R"c(
    void outputs(unsigned *a, unsigned *b) {
      unsigned old = *a;
      *a = 10;
      *b = old * 2u;
    }
    void bad(void) {
      unsigned a = 4, b = 0;
      outputs(&a, &b);
      char p[8];
      p[b - 1] = 0;
      p[b] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(NumericInputs, ReturnedAllocationGuardsTestTheEntryField) {
  const auto result = analyze(R"c(
    struct size { unsigned n; };
    char *make(struct size *s, unsigned limit) {
      if (s->n < limit) {
        char *p = malloc(s->n * 2u);
        s->n = limit;
        return p;
      }
      return 0;
    }
    void bad(void) {
      struct size s = {4};
      char *p = make(&s, 8);
      if (!p) return;
      p[7] = 0;
      p[8] = 0;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(NumericInputs, GlobalFieldDependenciesAreCapturedBeforeTheirWrites) {
  const auto result = analyze(R"c(
    struct size { unsigned n; };
    struct size *global;
    char *make(void) {
      char *p = malloc(global->n * 2u);
      global->n += 1;
      return p;
    }
    void bad(void) {
      global = malloc(sizeof *global);
      if (!global) return;
      global->n = 4;
      char *p = make();
      if (p) {
        p[7] = 0;
        p[8] = 0;
        free(p);
      }
      free(global);
      global = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(NumericInputs, ReusedCallInputDoesNotResizeAnEarlierAllocation) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    char *make(unsigned n) { return malloc(n * 2u); }
    void repeat(void) {
      char *first = 0;
      for (unsigned i = 0; i < 2; ++i) {
        char *p = make(i + 1);
        if (!p) { free(first); return; }
        if (first) {
          first[2] = 0;
          free(first);
        }
        first = p;
      }
      free(first);
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  const auto start = dump.find("function 'repeat':");
  ASSERT_NE(start, std::string::npos) << dump;
  // At the only access, first is the earlier two-byte allocation. Widening
  // can lose that extent, but it must not use the new four-byte allocation
  // to prove this access safe.
  EXPECT_NE(dump.substr(start).find("spatial: proven=0"), std::string::npos)
      << dump;
}

TEST(NumericInputs, AliasedOutputPathsRespectTheCalleesStatementOrder) {
  const auto result = analyze(R"c(
    void writes(unsigned *a, unsigned *b) { *b = 1; *a = 2; }
    void bad(void) {
      unsigned x;
      writes(&x, &x);
      int *badPointer = malloc(4);
      if (!badPointer) return;
      free(badPointer);
      if (x == 2) *badPointer = 1;
    }
    void good(void) {
      unsigned x;
      writes(&x, &x);
      int *goodPointer = malloc(4);
      if (!goodPointer) return;
      free(goodPointer);
      if (x == 1) *goodPointer = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  ASSERT_EQ(result.diagnostics.size(), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
  const auto &diagnostic = result.diagnostics.diagnostics().front();
  EXPECT_EQ(diagnostic.id, core::diag::UseAfterFree);
  EXPECT_EQ(diagnostic.message, "use of 'badPointer' after it was freed");
}

TEST(NumericInputs, DuplicateProjectedFactsRetainNumericOutputs) {
  const auto result = analyze(R"c(
    void outputs(unsigned *a, unsigned *b) { *a = 2; *b = 2; }
    void bad(void) {
      unsigned a, b;
      outputs(&a, &b);
      char p[2];
      p[a - 1] = 0;
      p[b] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
  const auto *summary = result.summary("outputs");
  ASSERT_NE(summary, nullptr);
  EXPECT_FALSE(
      summary->incomplete.contains("unsupported numeric output projection"));
}
