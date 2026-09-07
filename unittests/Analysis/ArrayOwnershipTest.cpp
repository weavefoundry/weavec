//===- ArrayOwnershipTest.cpp - RFC 0015 container contents ---------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace weavec::analysis {

static std::size_t countId(const test::AnalysisResult &result,
                           std::string_view id) {
  return static_cast<std::size_t>(
      std::ranges::count_if(result.diagnostics.diagnostics(),
                            [id](const core::Diagnostic &diagnostic) {
                              return diagnostic.id == id;
                            }));
}

static constexpr const char *Memory = R"c(
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
)c";

TEST(ArrayOwnership, ReleasingAnotherElementDoesNotEraseHistory) {
  const auto result = test::analyze(R"c(
void bad(char **a) { free(a[0]); free(a[1]); a[0][0] = 1; free(a[0]); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 1U);
}

TEST(ArrayOwnership, IndependentElementsMayEachBeReleasedOnce) {
  const auto result = test::analyze(R"c(
void clean(char **a) { free(a[0]); free(a[1]); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, AConstantIndexIsCapturedBeforeReassignment) {
  const auto result = test::analyze(R"c(
void bad(char **a) { int i = 0; free(a[i]); i = 1; a[0][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, SavedSymbolicIndicesRetainTheirEntryIdentity) {
  const auto result = test::analyze(R"c(
void bad(char **a, int i) { int old = i; free(a[i]); i = 9; a[old][0] = 1; }
void clean(char **a, int i) { free(a[i]); a[i+1][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, ReplacingAReleasedSlotDoesNotReviveAnOldAlias) {
  const auto result = test::analyze(R"c(
void bad(char **a, char *replacement) {
  char *old = a[0]; free(a[0]); a[0] = replacement; a[0][0] = 1; old[0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, InitializingOneCellDoesNotInitializeAnother) {
  const auto result = test::analyze(R"c(
void bad(char *p) { char *a[2]; a[0] = p; a[0][0] = 1; a[1][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseOfUninitialized), 1U);
}

TEST(ArrayOwnership, AggregateOmissionsAreNullAndGuardsRefineOnlyTheirCell) {
  const auto result = test::analyze(R"c(
void bad(char *p) { char *a[2] = {p}; a[0][0] = 1; a[1][0] = 1; }
void clean(void) {
  char *a[2]; a[0] = malloc(4); if (!a[0]) return;
  a[1] = malloc(4); if (!a[1]) { free(a[0]); return; }
  free(a[0]); a[1][0] = 1; free(a[1]);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::NullDereference), 1U);
  EXPECT_EQ(countId(result, core::diag::Leak), 0U);
}

TEST(ArrayOwnership, CompleteCopiesPreservePointerIdentity) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **source) {
  char *dest[2]; memcpy(dest, source, sizeof dest);
  free(source[0]); dest[0][0] = 1;
}
void clean(char **source) {
  char *dest[2]; memcpy(dest, source, sizeof dest);
  free(source[0]); dest[1][0] = 1; free(source[1]);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(ArrayOwnership, OverlappingMovesSnapshotEverySourceBeforeWriting) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void right(char **a) {
  char *old = a[1]; memmove(a+1, a, 2*sizeof *a); free(old); a[2][0] = 1;
}
void left(char **a) {
  char *old = a[1]; memmove(a, a+1, 2*sizeof *a); free(old); a[0][0] = 1;
}
void clean(char **a) {
  char *old = a[1]; memmove(a+1, a, 2*sizeof *a); free(old); a[1][0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 2U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(ArrayOwnership, SelfCopyAndZeroLengthDoNotLoseContents) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **a) { free(a[0]); memmove(a,a,2*sizeof *a); a[0][0] = 1; }
void zero(char **a, char **b) { free(a[0]); memcpy(a,b,0); a[0][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 2U);
}

TEST(ArrayOwnership, ACopyRetainsTheValueBeforeSourceReplacement) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **a, char *replacement) {
  char *old = a[0], *b[2]; memcpy(b,a,sizeof b); a[0] = replacement;
  free(old); b[0][0] = 1; a[0][0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, SelectedEffectsComposeThroughHelpers) {
  const auto result = test::analyze(R"c(
static void drop(char **a, int i) { free(a[i]); }
void bad(char **a) { drop(a,0); drop(a,1); a[0][0] = 1; }
void clean(char **a) { drop(a,0); a[1][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, CopyHelpersPublishFinalContents) {
  const auto result = test::analyze(std::string(Memory) + R"c(
static void copy(char **d, char **s) { memcpy(d,s,2*sizeof *d); }
static void replace(char **d, char **s, char *p) { memcpy(d,s,2*sizeof *d); d[0] = p; }
void bad(char **a, char **b) { copy(b,a); free(a[0]); b[0][0] = 1; }
void clean(char **a, char **b, char *p) { replace(b,a,p); free(a[0]); b[0][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, CallbackCellsUseTheirOwnTarget) {
  const auto result = test::analyze(R"c(
static void drop(void *p) { free(p); }
static void keep(void *p) { (void)p; }
void bad(char *p) { void (*table[2])(void *) = {drop, keep}; table[0](p); p[0] = 1; }
void clean(char *p) { void (*table[2])(void *) = {drop, keep}; table[1](p); p[0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(ArrayOwnership, NestedDimensionsHaveIndependentCells) {
  const auto result = test::analyze(R"c(
void bad(char *p, char *q) {
  char *a[2][2] = {{p,q},{0,0}}; free(a[0][0]); free(a[0][1]); a[0][0][0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, RecordElementsPreserveTheirPointerFields) {
  const auto result = test::analyze(std::string(Memory) + R"c(
struct item { char *p; int count; };
void bad(struct item *a) {
  struct item b[2]; char *old = a[0].p; memcpy(b,a,sizeof b); free(old); b[0].p[0] = 1;
}
void clean(struct item *a) { free(a[0].p); a[1].p[0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, SymbolicCopiesMaterializeKnownMembers) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **d, char **s, size_t n) {
  memcpy(d,s,n*sizeof *s); if (n > 0) { free(s[0]); d[0][0] = 1; }
}
void clean(char **d, char **s, size_t n) {
  memcpy(d,s,n*sizeof *s); if (n > 1) { free(s[0]); d[1][0] = 1; }
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, SymbolicCopyInputAndLengthSurviveLaterWrites) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **d, char **s, size_t n, char *p) {
  if (n == 0) return;
  memcpy(d,s,n*sizeof *s); char *old = s[0]; s[0] = p; n = 0;
  free(old); d[0][0] = 1; s[0][0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, SymbolicCopySummariesInstantiateCountsAndFinalOverrides) {
  const auto result = test::analyze(std::string(Memory) + R"c(
static void copy(char **d, char **s, size_t n) { memcpy(d,s,n*sizeof *s); }
static void replace(char **d, char **s, size_t n, char *p) {
  copy(d,s,n); if (n) d[0] = p;
}
void bad(char **s, char **d) { copy(d,s,2); free(s[0]); d[0][0] = 1; }
void clean(char **s, char **d, char *p) { replace(d,s,2,p); free(s[0]); d[0][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  ASSERT_TRUE(result.summary("copy"));
  EXPECT_EQ(result.summary("copy")->arrayCopies.size(), 1U);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, LargeCopiesAreSparseAndPreserveFarSelectedCells) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **d, char **s) { memcpy(d,s,1000000*sizeof *s); free(s[999999]); d[999999][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(ArrayOwnership, PartialCopiesExposeCoverageAndPreserveUnrelatedHistory) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **a, char **b, char **unrelated) {
  free(unrelated[0]); memcpy(a,b,sizeof *a-1); unrelated[0][0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 1U);
}

TEST(ArrayOwnership, SteppedAliasesAndAddressedElementsAgree) {
  const auto result = test::analyze(R"c(
static void drop(char **a) { free(a[0]); }
void bad(char **a) { char **q = a+1; free(q[0]); a[1][0] = 1; a[0][0] = 1; }
void addressed(char **a) { drop(&a[1]); a[1][0] = 1; a[0][0] = 1; }
void local(char *p, char *q) { char *a[2] = {p,q}; char **b=a+1; free(b[0]); a[1][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 3U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, UnknownWritesPreserveEarlierFreeEvidence) {
  const auto result = test::analyze(R"c(
void bad(char **a, int i, char *p) {
  char *old = a[0]; free(a[0]); a[i*i] = p; old[0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  // RFC 0017 represents the product selector; joining the old and new
  // cells is complete even when their membership is undecided.
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(ArrayOwnership, PartialInitializationIncludesRecordFields) {
  const auto result = test::analyze(R"c(
struct item { char *p; char *q; };
void bad(char *p) { struct item a[2]; a[0].p = p; a[1].p[0] = 1; }
void nil(char *p) { struct item a[2] = {{p}}; a[0].q[0] = 1; a[1].p[0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseOfUninitialized), 1U);
  EXPECT_EQ(countId(result, core::diag::NullDereference), 2U);
}

TEST(ArrayOwnership, GlobalCallbackElementsDoNotMixTargets) {
  const auto result = test::analyze(R"c(
static void drop(void *p) { free(p); }
static void keep(void *p) { (void)p; }
static void (*table[2])(void *) = {drop, keep};
void bad(char *p) { table[0](p); p[0] = 1; }
void clean(char *p) { table[1](p); p[0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(ArrayOwnership, CopyingCallbackArraysRetainsEachTarget) {
  const auto result = test::analyze(std::string(Memory) + R"c(
static void drop(void *p) { free(p); }
static void keep(void *p) { (void)p; }
void bad(char *p) {
  void (*a[2])(void *) = {drop, keep}, (*b[2])(void *);
  memcpy(b,a,sizeof a); b[0](p); p[0] = 1;
}
void clean(char *p) {
  void (*a[2])(void *) = {drop, keep}, (*b[2])(void *);
  memcpy(b,a,sizeof a); b[1](p); p[0] = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(ArrayOwnership, ConditionalCopiesDoNotReplaceUntouchedContents) {
  const auto result = test::analyze(std::string(Memory) + R"c(
static void maybe(char **d, char **s, size_t n, int c) {
  if (c) memcpy(d,s,n*sizeof *s);
}
void bad(char **d, char **s) { free(d[0]); maybe(d,s,2,0); d[0][0] = 1; }
void clean(char **d, char **s) { free(d[0]); maybe(d,s,2,1); d[0][0] = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, ReallocationKeepsChildrenButInvalidatesOldStorage) {
  const auto result = test::analyze(R"c(
void bad(char *p) {
  char **a = malloc(2*sizeof *a); if (!a) return;
  a[0] = p; char **old = a; char **b = realloc(a,4*sizeof *a);
  if (!b) { a[0][0] = 1; free(a); return; }
  free(p); b[0][0] = 1; old[0] = 0; free(b);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::UseAfterMove), 1U);
}

TEST(ArrayOwnership, CopyingOwnedPointersDoesNotCreateAdditionalAllocations) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void clean(void) {
  char *a[2] = {malloc(4),malloc(4)}, *b[2];
  memcpy(b,a,sizeof a); free(b[0]); free(b[1]);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::Leak), 0U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 0U);
}

TEST(ArrayOwnership, CompleteCleanupLoopsRetainEveryReleasedElement) {
  const auto result = test::analyze(R"c(
void bad(char **a) { for (int i=0; i<3; ++i) free(a[i]); a[0][0]=1; a[2][0]=1; }
void clean(char **a) { for (int i=0; i<3; ++i) free(a[i]); a[3][0]=1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 2U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 0U);
}

TEST(ArrayOwnership, CleanupHelpersPreserveLengthsAcrossCalls) {
  const auto result = test::analyze(R"c(
static void drop(char **a, size_t n) { for (size_t i=0; i<n; ++i) free(a[i]); }
void bad(char **a) { drop(a,3); a[2][0]=1; }
void zero(char **a) { drop(a,0); a[0][0]=1; }
void outside(char **a) { drop(a,3); a[3][0]=1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  ASSERT_TRUE(result.summary("drop"));
  EXPECT_EQ(result.summary("drop")->arrayReleases.size(), 1U);
}

TEST(ArrayOwnership, CleanupCanClearSlotsAndThenRepopulateThem) {
  const auto result = test::analyze(R"c(
static void clear(char **a, size_t n) {
  for (size_t i=0; i<n; ++i) { free(a[i]); a[i]=0; }
}
void clean(char **a, char *p) { clear(a,2); free(a[0]); a[0]=p; a[0][0]=1; }
void bad(char **a) { char *old=a[0]; clear(a,2); old[0]=1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 0U);
}

TEST(ArrayOwnership, AShortTraversalDoesNotClaimAnUnvisitedTail) {
  const auto result = test::analyze(R"c(
void clean(char **a) {
  for (int i=0; i<3; ++i) { if (i==1) break; free(a[i]); }
  a[2][0]=1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 0U);
}

TEST(ArrayOwnership, CleanupMustClearTheSameStorageWithoutSideEffects) {
  const auto result = test::analyze(R"c(
void advance(char **a, int n) {
  for (int i=0; i<n; ++i) { free(a[i]); (a++)[i]=0; }
}
void shifted(char **a, int n) {
  for (int i=0; i<n; ++i) { free(a[i]); (a+1)[i]=0; }
}
void indexed(char **a, int n) {
  for (int i=0; i<n; ++i) { free(a[i]); (a+i)[i]=0; }
}
)c");
  ASSERT_TRUE(result.ast);
  // None establishes the postcondition "every released cell is null".
  for (const auto *name : {"advance", "shifted", "indexed"}) {
    ASSERT_TRUE(result.summary(name));
    EXPECT_TRUE(result.summary(name)->arrayReleases.empty()) << name;
  }
}

TEST(ArrayOwnership, TraversalsAlsoSupportSeparatelyDeclaredCounters) {
  const auto result = test::analyze(R"c(
static void fill(char **a, size_t n) {
  size_t i; for (i=0; i<n; ++i) a[i]=malloc(4);
}
static void drop(char **a, size_t n) {
  size_t i; for (i=0; i<n; ++i) free(a[i]);
}
void clean(void) { char *a[3]; fill(a,3); drop(a,3); }
void bad(char **a) { drop(a,3); a[2][0]=1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 0U);
  EXPECT_EQ(countId(result, core::diag::Leak), 0U);
  ASSERT_TRUE(result.summary("drop"));
  EXPECT_EQ(result.summary("drop")->arrayReleases.size(), 1U);
}

TEST(ArrayOwnership, CleanupDischargesOwnedElementsExactlyOnce) {
  const auto result = test::analyze(R"c(
void clean(void) {
  char *a[2]={malloc(4),malloc(4)};
  for (int i=0; i<2; ++i) free(a[i]);
}
void bad(char *p) {
  char *a[2]={p,p}; for (int i=0; i<2; ++i) free(a[i]);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::Leak), 0U);
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, ProvedFillsInitializeIndependentAllocations) {
  const auto result = test::analyze(R"c(
static void fill(char **a, int n) { for (int i=0; i<n; ++i) a[i]=malloc(4); }
void clean(void) {
  char *a[4]; for (int i=0; i<4; ++i) a[i]=malloc(4);
  for (int i=0; i<4; ++i) free(a[i]);
}
void helper(void) { char *a[3]; fill(a,3); for (int i=0; i<3; ++i) free(a[i]); }
void bad(void) { char *a[2]; fill(a,2); free(a[0]); free(a[1]); a[0][0]=1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 0U);
  EXPECT_EQ(countId(result, core::diag::UseOfUninitialized), 0U);
  EXPECT_EQ(countId(result, core::diag::Leak), 0U);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
  ASSERT_TRUE(result.summary("fill"));
  EXPECT_EQ(result.summary("fill")->arrayFills.size(), 1U);
}

TEST(ArrayOwnership, ReturnedContainersPreserveConstantAndSymbolicCopies) {
  const auto result = test::analyze(std::string(Memory) + R"c(
static char **copy(char **source, size_t n) {
  char **b=malloc(n*sizeof *b); if (!b) return 0;
  memcpy(b,source,n*sizeof *b); return b;
}
void bad(char **a) { char **b=copy(a,3); if (!b) return; free(a[2]); b[2][0]=1; free(b); }
void clean(char **a) { char **b=copy(a,3); if (!b) return; free(a[2]); b[1][0]=1; free(b); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
  ASSERT_TRUE(result.summary("copy"));
  EXPECT_FALSE(result.summary("copy")->arrayCopies.empty());
}

TEST(ArrayOwnership, RangeInputsSurviveAnInterveningCallAndReplacement) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **a, char **b, char *p, size_t n) {
  if (n < 3) return;
  memcpy(b,a,n*sizeof *b);
  char *old=a[2]; a[2]=p; free(old); b[2][0]=1; a[2][0]=1;
}
void stepped(char **a, char **b, size_t n) {
  if (n < 2) return;
  char **q=a+1; memcpy(b,q,n*sizeof *b); free(a[2]); b[1][0]=1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 2U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, ReallocationDoesNotKeepLostChildrenAliveThroughSnapshots) {
  const auto result = test::analyze(R"c(
void bad(void) {
  char **a=malloc(2*sizeof *a); if (!a) return;
  a[0]=malloc(4); a[1]=malloc(4);
  char **b=realloc(a,4*sizeof *a);
  if (!b) { free(a[0]); free(a[1]); free(a); return; }
  free(b);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::Leak), 2U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, CopyingReferenceCountedPointersDoesNotRetainAShare) {
  const auto result = test::analyze(std::string(Memory) + R"c(
struct obj { int rc; };
static struct obj *ref(struct obj *p) { ++p->rc; return p; }
static void unref(struct obj *p) { if (--p->rc==0) free(p); }
void bad(struct obj *p) {
  struct obj *a[1]={p}, *b[1]; memcpy(b,a,sizeof b); unref(a[0]); unref(b[0]);
}
void clean(struct obj *p) {
  struct obj *a[2]={p,ref(p)}, *b[2]; memcpy(b,a,sizeof b);
  unref(b[0]); unref(b[1]);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::Leak), 0U);
}

TEST(ArrayOwnership,
     SymbolicOverlapFreezesDestinationsBeforeTheyBecomeSources) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(char **a, size_t n) {
  if (n<3 || n>(size_t)-1/sizeof *a) return; char *old=a[1];
  memmove(a+1,a,n*sizeof *a); a[1][0]=1; free(old); a[2][0]=1;
}
void clean(char **a, size_t n) {
  if (n<3 || n>(size_t)-1/sizeof *a) return; char *old=a[1];
  memmove(a+1,a,n*sizeof *a); free(old); a[1][0]=1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, TraversalsStillCheckContainerStorage) {
  const auto result = test::analyze(R"c(
void null_fill(void) { char **a=0; for(int i=0;i<2;++i) a[i]=0; }
void null_drop(void) { char **a=0; for(int i=0;i<2;++i) free(a[i]); }
void zero(void) { char **a=0; for(int i=0;i<0;++i) free(a[i]); }
void bounds(void) { char *a[2]={0}; for(int i=0;i<3;++i) a[i]=0; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::NullDereference), 2U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::OutOfBounds), 1U);
}

TEST(ArrayOwnership, RecordIndicesFreezeTheirChildStateBeforeReassignment) {
  const auto result = test::analyze(R"c(
struct box { char *p; };
void bad(struct box *a, int i) { int old=i; free(a[i].p); i=9; a[old].p[0]=1; }
void clean(struct box *a, int i) { free(a[i].p); ++i; a[i].p[0]=1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, ReturnedFilledContainersCarryInitializationAndResources) {
  const auto result = test::analyze(R"c(
static char **make(int n) {
  char **a=malloc(n*sizeof *a); if (!a) return 0;
  for(int i=0;i<n;++i) a[i]=malloc(4);
  return a;
}
void clean(void) {
  char **b=make(3); if(!b) return;
  for(int i=0;i<3;++i) free(b[i]); free(b);
}
void bad(void) {
  char **b=make(3); if(!b) return;
  free(b[0]); free(b[1]); free(b[2]); b[2][0]=1; free(b);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::Leak), 0U);
  EXPECT_EQ(countId(result, core::diag::DoubleFree), 0U);
  EXPECT_EQ(countId(result, core::diag::UseOfUninitialized), 0U);
  ASSERT_TRUE(result.summary("make"));
  EXPECT_FALSE(result.summary("make")->arrayFills.empty());
}

TEST(ArrayOwnership, UnrepresentableFinalCompositionsExposeCoverage) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void rewritten(char **d, char **s, char *p, size_t n) {
  s[0]=p; memcpy(d,s,n*sizeof *s);
}
void composed(char **a, char **b, char **c, size_t n) {
  memcpy(b,a,n*sizeof *b); memcpy(c,b,n*sizeof *c);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_GE(countId(result, core::diag::AnalysisIncomplete), 2U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  ASSERT_TRUE(result.summary("rewritten"));
  EXPECT_TRUE(result.summary("rewritten")->arrayCopies.empty());
}

TEST(ArrayOwnership, ShrinkingAContainerReportsOnlyUnreachableOwnedChildren) {
  const auto result = test::analyze(R"c(
void bad(void) {
  char **a=malloc(2*sizeof *a); if(!a) return;
  a[0]=malloc(4); a[1]=malloc(4);
  char **b=realloc(a,sizeof *a);
  if(!b) { free(a[0]); free(a[1]); free(a); return; }
  free(b[0]); free(b);
}
void clean(void) {
  char **a=malloc(2*sizeof *a); if(!a) return;
  a[0]=malloc(4); a[1]=malloc(4); char *saved=a[1];
  char **b=realloc(a,sizeof *a);
  if(!b) { free(a[0]); free(saved); free(a); return; }
  free(b[0]); free(saved); free(b);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::Leak), 1U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(ArrayOwnership, ChainedPointersIntoLocalArraysApplyTheirOffsetOnce) {
  const auto result = test::analyze(R"c(
void bad(char *p, char *q, char *r) {
  char *a[3]={p,q,r}; char **b=a+1; char **c=b+1;
  free(c[0]); a[2][0]=1;
}
void dereference(char *p, char *q, char *r) {
  char *a[3]={p,q,r}; char **b=a+1; char **c=b+1;
  free(*c); a[2][0]=1;
}
void clean(char *p, char *q, char *r) {
  char *a[3]={p,q,r}; char **b=a+1; char **c=b+1;
  free(*c); a[0][0]=1; a[1][0]=1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 2U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(ArrayOwnership, RangeFillsRespectTheExistingCellBudgetAndHistory) {
  std::string code = "void bounded(char **a) { free(a[40]);";
  for (unsigned i = 41; i < 72; ++i)
    code += "use(a[" + std::to_string(i) + "]);";
  code += "for (int i=0; i<32; ++i) a[i]=0; a[40][0]=1; }";
  const auto result = test::analyze(code);
  ASSERT_TRUE(result.ast);
  EXPECT_GE(countId(result, core::diag::AnalysisIncomplete), 1U);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

} // namespace weavec::analysis
