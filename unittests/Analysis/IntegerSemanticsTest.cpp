//===- IntegerSemanticsTest.cpp - Target C integers (RFC 0017) ------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include "gtest/gtest.h"

using namespace weavec;
using namespace weavec::test;

TEST(IntegerSemantics, CheckedAllocationFailureSurvivesReturnedPointers) {
  const auto result = analyze(R"c(
    void *calloc(size_t, size_t);
    void *direct(size_t n, size_t m) { return calloc(n,m); }
    void *local(size_t n, size_t m) { void *p=calloc(n,m); return p; }
    void good(void) {
      int *p=direct((size_t)-1,2);
      if(p) { free(p); *p=1; }
      p=local((size_t)-1,2);
      if(p) { free(p); *p=1; }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(messages(result.diagnostics));
  for (const auto *name : {"direct", "local"}) {
    const auto *summary = result.summary(name);
    ASSERT_TRUE(summary);
    for (const auto &source : summary->returns)
      if (source.isFresh())
        EXPECT_FALSE(source.when.integers.empty()) << name;
  }
}

TEST(IntegerSemantics, WrappedMemoryCopyDoesNotOverwriteTheUntouchedCell) {
  const auto result = analyze(R"c(
    void *memcpy(void *, const void *, size_t);
    void bad(void) {
      int *p=malloc(4); if(!p)return;
      int *a[1]={0}, *b[1]={p};
      size_t n=(size_t)-1/sizeof(p)+1;
      memcpy(b,a,n*sizeof(p));
      free(p); *b[0]=1;
    }
    void good(void) {
      int *p=malloc(4); if(!p)return;
      int *a[1]={0}, *b[1]={p};
      memcpy(b,a,sizeof(p));
      free(p); if(b[0]) *b[0]=1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(std::ranges::count(ids(result.diagnostics),
                               std::string(core::diag::UseAfterFree)),
            1);
  EXPECT_EQ(result.diagnostics.size(), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(IntegerSemantics, IncrementOutputsAndPostfixIndicesPreserveEntryValues) {
  const auto result = analyze(R"c(
    unsigned old(unsigned n) { return n++; }
    unsigned next(unsigned n) { return ++n; }
    struct index { unsigned n; };
    void put(char *p, struct index *i) { p[i->n++] = 0; }
    void bad(void) { char p[2]; struct index i={2}; put(p,&i); }
    void good(void) {
      char p[2]; struct index i={1}; put(p,&i);
      int *q=malloc(4); if(!q)return;
      free(q);
      if(old(4294967295u)!=4294967295u || next(4294967295u)!=0) *q=1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(std::ranges::count(ids(result.diagnostics),
                               std::string(core::diag::OutOfBounds)),
            1);
  EXPECT_EQ(std::ranges::count(ids(result.diagnostics),
                               std::string(core::diag::UseAfterFree)),
            0);
  ASSERT_TRUE(result.summary("old"));
  EXPECT_TRUE(result.summary("old")->numericOutputs.contains(
      core::SummaryPath::result()));
}

static unsigned countId(const AnalysisResult &result, std::string_view id) {
  return static_cast<unsigned>(
      std::ranges::count_if(result.diagnostics.diagnostics(),
                            [id](const core::Diagnostic &diagnostic) {
                              return diagnostic.id == id;
                            }));
}

TEST(IntegerSemantics, NarrowingCannotHideATemporalError) {
  const auto result = analyze(R"c(
    void bad(unsigned n) {
      if (n != 256) return;
      unsigned char k = n;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (k == 0) *p = 1;
    }
    void good(unsigned n) {
      if (n != 256) return;
      unsigned char k = n;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (k != 0) *p = 1;
    }
    void boolean(unsigned n) {
      if (n != 256) return;
      _Bool k = n;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (k == 0) *p = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(IntegerSemantics,
     IncrementUsesStorageWidthAndDoesNotInventAPositiveCount) {
  const auto result = analyze(R"c(
    void bad(unsigned n) {
      if (n != 4294967295u) return;
      n++;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (n == 0) *p = 1;
    }
    void good(unsigned char n) {
      if (n != 255) return;
      ++n;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (n != 0) *p = 1;
    }
    void signed_bad(int n) { if (n == 2147483647) ++n; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::InvalidIntegerOperation), 1U);
}

TEST(IntegerSemantics, InvalidArithmeticHasAnExactDiagnostic) {
  const auto result = analyze(R"c(
    int add(int n) { if (n != 2147483647) return 0; return n + 1; }
    int divzero(int n) { if (n != 0) return 0; return 2 / n; }
    int divoverflow(int n) { if (n != (-2147483647 - 1)) return 0; return n / -1; }
    int shift(int n) { if (n != 32) return 0; return 1 << n; }
    int negshift(int n) { if (n != -1) return 0; return n << 1; }
    unsigned wrap(unsigned n) { return n + 1; }
    int unknown(int n) { return n + 1; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (std::vector<std::string>{
                "2: invalid integer operation: signed integer overflow",
                "3: invalid integer operation: division by zero",
                "4: invalid integer operation: signed division overflow",
                "5: invalid integer operation: invalid shift count",
                "6: invalid integer operation: invalid signed left shift"}));
}

TEST(IntegerSemantics, ActualConvertedAllocationSizeIsUsed) {
  const auto result = analyze(R"c(
    void bad(unsigned n) {
      if (n != 256) return;
      char *p = malloc((unsigned char)n);
      if (p) { p[1] = 0; free(p); }
    }
    void good(unsigned n) {
      if (n != 257) return;
      char *p = malloc((unsigned char)n);
      if (p) { p[0] = 0; free(p); }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
}

TEST(IntegerSemantics, RepeatedProductsAndVlaBoundsAreNotLost) {
  const auto result = analyze(R"c(
    void product(unsigned rows, unsigned cols) {
      char *p = malloc(rows * cols);
      if (p) { p[rows * cols] = 0; free(p); }
    }
    void vla(unsigned n) { if (!n) return; char a[n]; a[n] = 0; }
    void good(unsigned n) { if (!n) return; char a[n]; a[n - 1] = 0; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U);
}

TEST(IntegerSemantics, ReturnedAndOutputValuesUseTheirDeclaredTypes) {
  const auto result = analyze(R"c(
    unsigned char narrow(unsigned n) { return n; }
    void output(unsigned n, unsigned *r) { *r = (unsigned char)n; }
    void returned(unsigned n) {
      if (n != 256) return;
      int *p = malloc(4); if (!p) return;
      free(p); if (narrow(n) == 0) *p = 1;
    }
    void good(unsigned n) {
      if (n != 256) return;
      unsigned r; output(n, &r);
      int *p = malloc(4); if (!p) return;
      free(p); if (r != 0) *p = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  ASSERT_TRUE(result.summary("narrow"));
  EXPECT_TRUE(result.summary("narrow")->numericOutputs.contains(
      core::SummaryPath::result()));
}

TEST(IntegerSemantics, BitfieldStorageWidthIsNotThePromotedExpressionWidth) {
  const auto result = analyze(R"c(
    struct bits { unsigned n : 3; };
    void bad(void) {
      struct bits b; b.n = 8;
      int *p = malloc(4); if (!p) return;
      free(p); if (b.n == 0) *p = 1;
    }
    void good(void) {
      struct bits b; b.n = 7; b.n++;
      int *p = malloc(4); if (!p) return;
      free(p); if (b.n != 0) *p = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(IntegerSemantics, NarrowingConditionsRemainConditionalAcrossCalls) {
  const auto result = analyze(R"c(
    void maybe_free(int *p, unsigned n) { if ((unsigned char)n == 0) free(p); }
    void good(void) {
      int *p = malloc(4); if (!p) return;
      maybe_free(p, 257); *p = 1; free(p);
    }
    void bad(void) {
      int *p = malloc(4); if (!p) return;
      maybe_free(p, 256); *p = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 0U);
}

TEST(IntegerSemantics,
     MinimumAndConditionalRequirementsComposeThroughWrappers) {
  const auto result = analyze(R"c(
    void fill(char *p, unsigned n, unsigned cap) {
      for (unsigned i = 0; i < n && i < cap; ++i) p[i] = 0;
    }
    void wrapper(char *p, unsigned n, unsigned cap) { fill(p, n, cap); }
    void conditional(char *p, unsigned n, unsigned m) { if (n < m) p[n] = 0; }
    void bad(void) { char b[2]; wrapper(b,3,3); }
    void good(void) { char b[2]; wrapper(b,20,2); conditional(b,2,2); }
    void also_bad(void) { char b[2]; conditional(b,2,3); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(IntegerSemantics, FlexibleTailKeepsItsBoundsAndEnclosingLifetime) {
  const auto result = analyze(R"c(
    struct v { unsigned n; int a[]; };
    void bounds_bad(void) {
      struct v *p = malloc(sizeof *p + 2*sizeof(int)); if (!p) return;
      int *q = p->a; q[2] = 0; free(p);
    }
    void good(void) {
      struct v *p = malloc(sizeof *p + 2*sizeof(int)); if (!p) return;
      int *q = p->a; q[1] = 0; free(p);
    }
    void lifetime_bad(void) {
      struct v *p = malloc(sizeof *p + 2*sizeof(int)); if (!p) return;
      int *q = p->a; free(p); q[0] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(IntegerSemantics, CompoundAssignmentsComputeBeforeStorageConversion) {
  const auto result = analyze(R"c(
    void bad(void) {
      unsigned char n = 250; n += 6;
      int *p = malloc(4); if (!p) return;
      free(p); if (n == 0) *p = 1;
    }
    void good(void) {
      unsigned n = 4294967295u; n *= 2;
      n >>= 1; n ^= 2147483647u;
      int *p = malloc(4); if (!p) return;
      free(p); if (n != 0) *p = 1;
    }
    void invalid(void) { int n = 2147483647; n += 2; }
    void bitfield(void) {
      struct { unsigned n : 3; } b; b.n = 7;
      unsigned old = b.n++;
      int *p = malloc(4); if (!p) return;
      free(p); if (old != 7 || b.n != 0) *p = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::InvalidIntegerOperation), 1U);
}

TEST(IntegerSemantics, SwitchUsesTheConvertedFullWidthValue) {
  const auto result = analyze(R"c(
    void bad(unsigned long long n) {
      if (n != 18446744073709551615ull) return;
      int *p = malloc(4); if (!p) return; free(p);
      switch (n) { case -1: *p = 1; break; default: break; }
    }
    void good(unsigned long long n) {
      if (n != 18446744073709551615ull) return;
      int *p = malloc(4); if (!p) return; free(p);
      switch (n) { case -1: break; default: *p = 1; }
    }
    void narrow(unsigned n) {
      if (n != 256) return;
      int *p = malloc(4); if (!p) return; free(p);
      switch ((unsigned char)n) { case 0: break; default: *p = 1; }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(IntegerSemantics, NestedVlaDimensionsAndSizeofUseDeclarationValues) {
  const auto result = analyze(R"c(
    void bad(unsigned rows, unsigned cols) {
      if (!rows || !cols) return;
      int a[rows][cols];
      a[rows][0] = 1; a[0][cols] = 1;
    }
    void captured(void) {
      unsigned rows = 2, cols = 3;
      int a[rows][cols]; rows = 8; cols = 9;
      int *p = malloc(4); if (!p) return; free(p);
      if (sizeof a != 24 || sizeof a[0] != 12) *p = 1;
      a[1][2] = 1; a[1][3] = 1;
    }
    void good(unsigned n, unsigned m) {
      if (!n || !m) return;
      int a[n][m]; a[n-1][m-1] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 3U)
      << ::testing::PrintToString(messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 0U);
}

TEST(IntegerSemantics, CheckedAllocatorsCannotSucceedWithAWrappedProduct) {
  const auto result = analyze(R"c(
    void *calloc(unsigned long, unsigned long);
    void *reallocarray(void *, unsigned long, unsigned long);
    void good(void) {
      int *p = malloc(4); if (!p) return; free(p);
      char *q = calloc((unsigned long)-1, 2);
      if (q) { *p = 1; free(q); }
    }
    void preserves(void) {
      int *p = malloc(4); if (!p) return;
      int *q = reallocarray(p, (unsigned long)-1, 2);
      if (!q) { *p = 1; free(p); }
    }
    void product(unsigned long n, unsigned long m) {
      char *p = calloc(n,m); if (!p) return;
      p[n*m] = 0; free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 0U);
  EXPECT_EQ(countId(result, core::diag::UseAfterMove), 0U);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(IntegerSemantics,
     OverflowBuiltinsStoreTheConvertedValueAndReturnOverflow) {
  const auto result = analyze(R"c(
    void bad(void) {
      unsigned char out;
      int overflow = __builtin_add_overflow(255u, 1u, &out);
      int *p = malloc(4); if (!p) return; free(p);
      if (overflow && out == 0) *p = 1;
    }
    void good(void) {
      unsigned long long n = 18446744073709551615ull;
      int overflow = __builtin_add_overflow(n, -1, &n);
      int *p = malloc(4); if (!p) return; free(p);
      if (overflow || n != 18446744073709551614ull) *p = 1;
    }
    int product(unsigned n, unsigned m, unsigned *out) {
      return __builtin_mul_overflow(n,m,out);
    }
    void helper(void) {
      unsigned out;
      int overflow = product(65536,65536,&out);
      int *p = malloc(4); if (!p) return; free(p);
      if (!overflow || out != 0) *p = 1;
    }
    void checked(unsigned n, unsigned m) {
      unsigned size;
      if (__builtin_mul_overflow(n,m,&size)) return;
      char *p = malloc(size); if (p) { p[size] = 1; free(p); }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
  EXPECT_EQ(countId(result, core::diag::InvalidIntegerOperation), 0U);
}

TEST(IntegerSemantics, FullWidthUnsignedIndicesDoNotBecomeNegativeOrUnknown) {
  const auto result = analyze(R"c(
    void bad(void) {
      char a[4]; a[18446744073709551615ull] = 0;
    }
    void bounded(unsigned long long n) {
      if (n != 18446744073709551615ull) return;
      int *p = malloc(16); if (!p) return; p[n] = 0; free(p);
    }
    void good(void) { char a[4]; a[(unsigned char)18446744073709551615ull & 3] = 0; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U);
}

TEST(IntegerSemantics, RequirementsRetainTheFirstAccessedByte) {
  const auto result = analyze(R"c(
    void put(char *p, int i) { p[i] = 1; }
    void wrap(char *p, int i) { put(p, i); }
    void bad(void) { char a[4]; wrap(a, -1); }
    void good(void) { char a[4]; wrap(a + 1, -1); }
    void *memset(void *, int, size_t);
    void empty(void) { char a[4]; memset(a, 0, 0); }
  )c");
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
}

TEST(IntegerSemantics, CheckedProductGuardsPreserveMathematicalBounds) {
  const auto result = analyze(R"c(
    void guarded(int n, int m) {
      if (n < 0 || m <= 0 || n > 2147483647 / m) return;
      int size = n * m;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (size < 0) *p = 1;
    }
    void builtin(int n, int m) {
      if (n < 0 || m < 0) return;
      int size;
      if (__builtin_mul_overflow(n, m, &size)) return;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (size < 0) *p = 1;
    }
    void wrapped(unsigned n, unsigned m) {
      if (n != 65536 || m != 65536) return;
      unsigned size = n * m;
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (size == 0) *p = 1;
    }
  )c");
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::InvalidIntegerOperation), 0U);
}

TEST(IntegerSemantics, ReassignedInputsRetainTheirEntryIdentity) {
  const auto result = analyze(R"c(
    unsigned char old(unsigned n) {
      unsigned saved = n; n = 1; return saved;
    }
    void bad(void) {
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (old(256) == 0) *p = 1;
    }
    void good(void) {
      int *p = malloc(sizeof *p); if (!p) return;
      free(p); if (old(256) != 0) *p = 1;
    }
  )c");
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(IntegerSemantics, FlexibleTailAndElementOffsetsSurviveReturns) {
  const auto result = analyze(R"c(
    struct block { int count; double alignment; int data[]; };
    static int *tail(void) {
      struct block *b = malloc(sizeof *b + 2 * sizeof(int));
      if (!b) return NULL;
      return b->data;
    }
    void bad(void) { int *p = tail(); if (p) { p[2] = 1; } }
    void good(void) { int *p = tail(); if (p) { p[1] = 1; } }
    void shifted(void) {
      struct block *b = malloc(sizeof *b + 2 * sizeof(int));
      if (!b) return;
      int *q = b->data + 1; q[1] = 1; free(b);
    }
    void small(void) {
      struct block *b = malloc(4); if (!b) return;
      int *q = b->data; q[0] = 1; free(b);
    }
    struct fixed { int pad; int data[2]; };
    void fixed_bound(void) {
      struct fixed *b = malloc(100); if (!b) return;
      b->data[2] = 1; free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 4U);
}

TEST(IntegerSemantics, SizedFieldInferenceRetainsModularMultiplication) {
  const auto result = analyze(R"c(
    struct vec { int *items; size_t cap; };
    void init(struct vec *v, size_t n) {
      v->items = malloc(n * sizeof *v->items); v->cap = n;
    }
    void bad(void) {
      struct vec v; init(&v, (size_t)1 << 62);
      if (v.items) v.items[0] = 1;
      free(v.items);
    }
    void good(void) {
      struct vec v; init(&v, 2);
      if (v.items) v.items[1] = 1;
      free(v.items);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
}

TEST(IntegerSemantics, PostfixRequirementPreservesThePreWriteBranch) {
  const auto result = analyze(R"c(
    struct bag { char items[8]; unsigned n; };
    void put(struct bag *b) { if (b->n == 8) return; b->items[b->n++] = 0; }
  )c");
  ASSERT_TRUE(result.ast);
  const auto *summary = result.summary("put");
  ASSERT_TRUE(summary);
  ASSERT_TRUE(summary->requiresExtent.contains(0));
  EXPECT_FALSE(summary->requiresExtent.at(0).empty());
  for (const auto &requirement : summary->requiresExtent.at(0))
    EXPECT_FALSE(requirement.when.trivial());
  EXPECT_FALSE(
      summary->incomplete.contains("unsupported extent requirement condition"));
}

TEST(IntegerSemantics, AbstractEndpointsAreNotReachableBoundaryWitnesses) {
  const auto result = analyze(R"c(
    void unknown(int n) {
      char a[8]; for (int i=0; i<n; ++i) a[i]=0;
    }
    void masked(unsigned n) { char a[8]; a[n & 127u]=0; }
    void explicit_bound(int i) { char a[8]; if (i>=0 && i<=8) a[i]=0; }
    void definite(unsigned i) { char a[8]; if(i>=8) a[i]=0; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(IntegerSemantics, ExhaustedExpressionsRetainExplicitMissingCoverage) {
  const auto result = analyze(R"c(
    void large(size_t n) {
      char *p = malloc(n+n+n+n+n+n+n+n+n+n+n+n+n+n+n+n+n);
      if (p) { p[0]=0; free(p); }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(std::ranges::any_of(
      result.diagnostics.diagnostics(), [](const core::Diagnostic &diagnostic) {
        return diagnostic.id == core::diag::AnalysisIncomplete &&
               diagnostic.message ==
                   "analysis is incomplete: integer expression limit reached";
      }));
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U);
}
