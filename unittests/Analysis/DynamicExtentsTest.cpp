//===- DynamicExtentsTest.cpp - VLA and flexible tails (RFC 0017) ---------===//
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
  return static_cast<unsigned>(
      std::ranges::count_if(result.diagnostics.diagnostics(),
                            [id](const core::Diagnostic &diagnostic) {
                              return diagnostic.id == id;
                            }));
}

TEST(DynamicExtents, FixedOuterDimensionRetainsItsOwnBound) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 3;
      int a[2][n];
      a[1][2] = 1;
      a[2][0] = 1;
      a[0][3] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, FixedInnerDimensionRetainsItsOwnBound) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 3;
      int a[n][2];
      a[2][1] = 1;
      a[3][0] = 1;
      a[0][2] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, RepresentableMultidimensionalAccessIsProven) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 2, m = 3;
      int a[n][m];
      a[1][2] = 1;
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U);
  EXPECT_NE(dump.find("spatial: proven=2 violation=0 unresolved=0"),
            std::string::npos)
      << dump;
}

TEST(DynamicExtents, OverflowingElementByteProductCannotBeProven) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    void test(void) {
      size_t n = (size_t)1 << (sizeof(size_t) * 8 - 2);
      int a[n];
      a[0] = 1;
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U);
  EXPECT_GT(countId(result, core::diag::AnalysisIncomplete), 0U);
  EXPECT_NE(dump.find("spatial: proven=0 violation=0 unresolved=1"),
            std::string::npos)
      << dump;
}

TEST(DynamicExtents, OverflowingFixedOuterByteProductCannotBeProven) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    void test(void) {
      size_t n = (size_t)1 << (sizeof(size_t) * 8 - 2);
      char a[4][n];
      a[0][0] = 1;
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U);
  EXPECT_GT(countId(result, core::diag::AnalysisIncomplete), 0U);
  EXPECT_NE(dump.find("spatial: proven=0 violation=0 unresolved=2"),
            std::string::npos)
      << dump;
}

TEST(DynamicExtents, ByteOverflowPreservesIndependentDimensionViolations) {
  const auto result = analyze(R"c(
    void test(void) {
      size_t n = (size_t)1 << (sizeof(size_t) * 8 - 2);
      char a[4][n];
      a[4][0] = 1;
      a[0][n] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 2U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, PossiblyOverflowingByteProductRemainsUnresolved) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    void test(size_t n) {
      if (!n) return;
      int a[n];
      a[0] = 1;
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::InvalidIntegerOperation), 0U);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U);
  EXPECT_NE(dump.find("spatial: proven=0 violation=0 unresolved=1"),
            std::string::npos)
      << dump;
}

TEST(DynamicExtents, NonpositiveDimensionsDoNotInventStorage) {
  for (const auto *bound : {"0", "-1", "(unsigned char)256"}) {
    SCOPED_TRACE(bound);
    std::string dump;
    llvm::raw_string_ostream stream(dump);
    const auto result =
        analyze("void test(void) { int n = " + std::string(bound) +
                    "; int a[n]; a[0] = 1; }",
                {.dumpStream = &stream});
    ASSERT_TRUE(result.ast);
    EXPECT_EQ(countId(result, core::diag::InvalidIntegerOperation), 1U);
    EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U);
    EXPECT_NE(dump.find("spatial: proven=0 violation=0 unresolved=1"),
              std::string::npos)
        << dump;
  }
}

TEST(DynamicExtents, InvalidInnerDimensionKeepsTheFixedOuterViolation) {
  const auto result = analyze(R"c(
    void test(void) {
      int n = 0;
      int a[2][n];
      a[2][0] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::InvalidIntegerOperation), 1U);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
}

TEST(DynamicExtents, TypedefSizeIsCapturedBeforeItsVariablesAreDeclared) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 2;
      typedef int row[n];
      n = 5;
      row a[3];
      row *p = &a[0];
      n = 7;
      int *dead = malloc(sizeof *dead); if (!dead) return; free(dead);
      if (sizeof(row) != 2 * sizeof(int) ||
          sizeof a != 6 * sizeof(int) ||
          sizeof *p != 2 * sizeof(int)) *dead = 1;
      a[2][1] = 1;
      a[2][2] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 0U)
      << ::testing::PrintToString(messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
}

TEST(DynamicExtents, TypedefUsesAndShadowingHaveSeparateSnapshots) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 2;
      typedef int row[n];
      row first;
      n = 5;
      row second;
      first[1] = 1; first[2] = 1;
      second[1] = 1; second[2] = 1;
      {
        typedef int row[n];
        n = 9;
        row inner;
        inner[4] = 1; inner[5] = 1;
      }
      row last;
      last[1] = 1; last[2] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 4U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, PointerToVlaCapturesSizeWithoutOwningAnArray) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 3;
      int (*p)[n];
      n = 7;
      int *dead = malloc(sizeof *dead); if (!dead) return; free(dead);
      if (sizeof *p != 3 * sizeof(int) || sizeof p != sizeof(void *))
        *dead = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 0U)
      << ::testing::PrintToString(messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::UseOfUninitialized), 0U);
}

TEST(DynamicExtents, PointerToVlaRetainsSizeAndLifetimeAfterRelease) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 3;
      int (*p)[n] = malloc(3 * sizeof(int)); if (!p) return;
      n = 7;
      (*p)[2] = 1;
      (*p)[3] = 1;
      free(p);
      int *dead = malloc(sizeof *dead); if (!dead) return; free(dead);
      if (sizeof *p != 3 * sizeof(int)) *dead = 1;
      (*p)[0] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, PointerRowTypeDoesNotProveTheBackingAllocationFits) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 3;
      int (*p)[n] = malloc(1); if (!p) return;
      p[0][0] = 1;
      free(p);
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_NE(dump.find("spatial: proven=0"), std::string::npos) << dump;
}

TEST(DynamicExtents, LoopRedeclarationCannotReuseAnEarlierPositiveDimension) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    void test(void) {
      for (int n = 2; n >= 0; --n) {
        int a[n];
        a[0] = 1;
      }
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_NE(dump.find("spatial: proven=0 violation=0 unresolved=1"),
            std::string::npos)
      << dump;
}

TEST(DynamicExtents, TypedefOutsideALoopDoesNotResizeOnEachIteration) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 2;
      typedef int row[n];
      for (n = 3; n < 6; ++n) {
        row a;
        a[1] = 1;
        a[2] = 1;
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, SideEffectingDimensionsCannotProveStorage) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 2;
      int a[n++];
      a[0] = 1;
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countId(result, core::diag::AnalysisIncomplete), 0U);
  EXPECT_NE(dump.find("spatial: proven=0 violation=0 unresolved=1"),
            std::string::npos)
      << dump;
}

TEST(DynamicExtents, SideEffectingDimensionsDoNotFabricateSizeof) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 2;
      int a[n++];
      int *dead = malloc(sizeof *dead); if (!dead) return; free(dead);
      if (sizeof a == 2 * sizeof(int)) *dead = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, PointerInitializerCannotReplaceItsEarlierDimensionValue) {
  const auto result = analyze(R"c(
    void test(void) {
      unsigned n = 2;
      int (*p)[n] = (n = 3, malloc(3 * sizeof(int)));
      int *dead = malloc(sizeof *dead); if (!dead) { free(p); return; }
      free(dead);
      if (sizeof *p == 2 * sizeof(int)) *dead = 1;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, FlexibleTailUsesFieldOffsetIncludingRecordPadding) {
  const auto result = analyze(R"c(
    struct padded { double alignment; char flag; int data[]; };
    _Static_assert(sizeof(struct padded) ==
                   __builtin_offsetof(struct padded, data) + sizeof(int),
                   "test requires trailing record padding");
    void test(void) {
      struct padded *p = malloc(sizeof *p); if (!p) return;
      p->data[0] = 1;
      p->data[1] = 1;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, FlexibleTailRequiresTheWholeNonByteElement) {
  const auto result = analyze(R"c(
    struct block { char tag; long long data[]; };
    void test(void) {
      struct block *p = malloc(__builtin_offsetof(struct block, data) +
                               sizeof(long long) + 1);
      if (!p) return;
      long long *tail = p->data;
      tail[0] = 1;
      tail[1] = 1;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, FlexibleTailBeforeTheAllocationEndRetainsLifetime) {
  const auto result = analyze(R"c(
    struct block { char tag; long long data[]; };
    void test(void) {
      struct block *p = malloc(__builtin_offsetof(struct block, data) - 1);
      if (!p) return;
      long long *tail = p->data;
      tail[0] = 1;
      free(p);
      tail[0] = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countId(result, core::diag::OutOfBounds), 0U);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(messages(result.diagnostics));
}

TEST(DynamicExtents, FlexibleTailCannotUseAnUncheckedSiblingCountAsStorage) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  const auto result = analyze(R"c(
    struct block { unsigned count; int data[]; };
    void test(struct block *p) {
      if (!p) return;
      p->count = 100;
      p->data[0] = 1;
    }
  )c",
                              {.dumpStream = &stream});
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 0U);
  EXPECT_NE(dump.find("spatial: proven=0"), std::string::npos) << dump;
}
