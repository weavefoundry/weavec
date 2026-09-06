//===- PointerIdentityTest.cpp - RFC 0014 pointer and call identities -----===//
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

static constexpr const char *Callbacks = R"c(
static void drop(void *p) { free(p); }
static void keep(void *p) { (void)p; }
)c";

TEST(PointerIdentity, AStoredTargetDoesNotAcquireAnUnrelatedFunctionsEffects) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void (*unrelated)(void *) = drop;
void clean(void) {
  int *p = malloc(sizeof *p); if (!p) return;
  void (*fn)(void *) = keep; fn(p); *p = 1; free(p);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(PointerIdentity, ARepeatedCallbackReleaseReportsOneDoubleFree) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void bad(int *p) { void (*fn)(void *) = drop; fn(p); fn(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"double-free"});
}

TEST(PointerIdentity, NullCallbacksDoNotPassAsCheckedCalls) {
  const auto result = test::analyze(R"c(
void bad(int *p) { void (*fn)(void *) = 0; fn(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::NullDereference), 1U);
}

TEST(PointerIdentity, ANonNullGuardRemovesTheNullCallbackAlternative) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void clean(int *p, int select) {
  void (*fn)(void *) = select ? keep : 0;
  if (fn) { fn(p); use(p); }
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(PointerIdentity, HelperContextsKeepEachCallbackWithItsUserdata) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
static void invoke(void (*fn)(void *), void *p) { fn(p); }
void clean(void) {
  int *p = malloc(sizeof *p), *q = malloc(sizeof *q);
  if (!p || !q) { free(p); free(q); return; }
  invoke(keep, p); invoke(drop, q); *p = 1; free(p);
}
void bad(void) {
  int *p = malloc(sizeof *p); if (!p) return;
  invoke(drop, p); *p = 1;
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
  const auto exports = result.analyzer->exports();
  EXPECT_EQ(exports.functions.at("invoke").specializations.size(), 2U);
}

TEST(PointerIdentity, CallbackBodiesAreCheckedInTheRequestedContext) {
  const auto result = test::analyze(R"c(
static void drop(void *p) { free(p); }
static void invoke(void (*fn)(void *), int *p) { fn(p); *p = 1; }
void caller(int *p) { invoke(drop, p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(PointerIdentity, NestedForwardersAndReturnsPreserveTargets) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
static void invoke(void (*fn)(void *), void *p) { fn(p); }
typedef void (*Callback)(void *);
static Callback choose(void) { return drop; }
static void forward(Callback fn, void *data) { invoke(fn, data); }
void bad(int *p) { Callback fn = choose(); forward(fn, p); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnnotationRequired), 0U);
}

TEST(PointerIdentity,
     CopiedAndReturnedCallbackParametersRetainTheirInputPaths) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
typedef void (*Callback)(void *);
static Callback identity(Callback fn) { Callback saved = fn; return saved; }
static void copied(Callback fn, void *p) { Callback saved = fn; saved(p); }
void bad(int *p) { copied(identity(drop), p); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
}

TEST(PointerIdentity, CallbackFieldsSurviveRecordCopiesAndOutParameters) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
struct hook { void (*fn)(void *); void *data; };
static void install(struct hook *h) { h->fn = drop; }
void bad(int *p) { struct hook a = {keep, p}; install(&a); struct hook b = a; b.fn(b.data); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnnotationRequired), 0U);
}

TEST(PointerIdentity, UnknownTargetsRemainUnknownDespiteAddressTakenFunctions) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void (*unrelated)(void *) = keep;
void boundary(void (*fn)(void *), void *p) { fn(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::AnnotationRequired), 1U);
  const auto strict = test::analyze(std::string(Callbacks) + R"c(
void boundary(void (*fn)(void *), void *p) { fn(p); }
)c",
                                    AnalysisOptions{.strictExterns = true});
  ASSERT_TRUE(strict.ast);
  EXPECT_EQ(countId(strict, core::diag::UnsafeOperation), 1U);
}

TEST(PointerIdentity, ConditionalTargetsKeepAllPossibleEffects) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void bad(int *p, int select) { void (*fn)(void *) = select ? keep : drop; fn(p); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(PointerIdentity, HelpersRetainKnownEffectsAlongsideAnUnknownTarget) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
static void invoke(void (*fn)(void *), void *p) { fn(p); }
void bad(int *p, int select, void (*unknown)(void *)) {
  invoke(select ? drop : unknown, p); use(p);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnnotationRequired), 1U);
}

TEST(PointerIdentity, UnknownAssignmentDoesNotResurrectAGlobalInitializer) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void (*global)(void *) = keep;
void boundary(void (*unknown)(void *), int *p) { global = unknown; global(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::AnnotationRequired), 1U);
}

TEST(PointerIdentity,
     GlobalCallbackWritesRemainPossibleAtLaterFunctionEntries) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void (*global)(void *) = keep;
static void set_drop(void) { global = drop; }
void bad(int *p) { global(p); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(PointerIdentity, GlobalCallbackInferenceDoesNotDependOnFunctionOrder) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
void (*global)(void *) = keep;
static void invoke(int *p) { global(p); }
void bad(int *p) { invoke(p); use(p); }
static void set_drop(void) { global = drop; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
}

TEST(PointerIdentity, FunctionsReturningAGlobalCallbackRetainItsTargets) {
  const auto result = test::analyze(std::string(Callbacks) + R"c(
typedef void (*Callback)(void *);
static Callback global = drop;
static Callback get(void) { return global; }
void bad(int *p) { Callback fn = get(); fn(p); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
}

TEST(PointerIdentity, PointerInequalitySelectsTheNonConsumingHelperPath) {
  const auto result = test::analyze(R"c(
static void release_same(int *p, int *q) { if (p == q) free(p); }
void clean(int *p, int *q) { if (p != q) { release_same(p, q); use(p); free(p); } }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
  const auto *summary = result.summary("release_same");
  ASSERT_NE(summary, nullptr);
  EXPECT_EQ(summary->effectOf(core::SummaryPath::param(0)).when.pointers.size(),
            1U);
}

TEST(PointerIdentity, PointerEqualityStillDiagnosesActualConsumption) {
  const auto result = test::analyze(R"c(
static void release_same(int *p, int *q) { if (p == q) free(p); }
void bad(int *p) { release_same(p, p); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(PointerIdentity, AComparisonAfterTheCallCanRefuteAConditionalConsume) {
  const auto result = test::analyze(R"c(
static void release_same(int *p, int *q) { if (p == q) free(p); }
void clean(int *p, int *q) { release_same(p, q); if (p != q)
  use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::messages(result.diagnostics),
            (std::vector<std::string>{"3: use of 'p' after it was freed",
                                      "3: use of 'q' after it was freed"}));
}

TEST(PointerIdentity, OverwrittenComparisonOperandsUseTheIncomingValues) {
  const auto result = test::analyze(R"c(
static void replace_same(int **p, int *q, int *replacement) {
  if (*p == q) { free(*p); *p = replacement; }
}
void clean(int *p, int *q, int *replacement) {
  if (p != q) { replace_same(&p, q, replacement); use(p); }
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(PointerIdentity, PointerGuardsSurviveCopiesOfTheComparedValues) {
  const auto result = test::analyze(R"c(
static void release_same(int *p, int *q) { if (p == q) free(p); }
void clean(int *p, int *q) { int *saved = p; if (p != q) { release_same(saved, q); use(saved); } }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

static constexpr const char *Memory = R"c(
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
)c";

TEST(PointerIdentity, MemcpyCopiesPointerIdentityAndMoveState) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void bad(void) { int *p = malloc(sizeof *p), *q; if (!p) return; memcpy(&q, &p, sizeof p); free(p); *q = 1; }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
}

TEST(PointerIdentity, MemmoveCopiesCompleteRecordsAndNestedPointers) {
  const auto result = test::analyze(std::string(Memory) + R"c(
struct inner { int *p; }; struct outer { struct inner in; int count; };
void bad(int *p) { struct outer a = {{p}, 1}, b; memmove(&b, &a, sizeof a); free(a.in.p); use(b.in.p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::UseOfUninitialized), 0U);
}

TEST(PointerIdentity, CompleteRecordCopiesKeepChildBounds) {
  const auto result = test::analyze(std::string(Memory) + R"c(
struct box { int *p; };
void bad(void) {
  struct box a = {malloc(2 * sizeof(int))}, b;
  if (!a.p) return;
  memcpy(&b, &a, sizeof a); b.p[2] = 1; free(a.p);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"out-of-bounds"});
}

TEST(PointerIdentity, CompleteCopiesCarryCallbackTargets) {
  const auto result = test::analyze(std::string(Memory) + Callbacks + R"c(
void bad(int *p) { void (*fn)(void *) = drop, (*copy)(void *); memcpy(&copy, &fn, sizeof fn); copy(p); use(p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countId(result, core::diag::AnnotationRequired), 0U);
}

TEST(PointerIdentity, CompleteCopiesThroughHelpersExportPointerStores) {
  const auto result = test::analyze(std::string(Memory) + R"c(
static void copy_pointer(int **dest, int **source) {
  memcpy(dest, source, sizeof *source);
}
void bad(int *p) { int *q; copy_pointer(&q, &p); free(p); use(q); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
}

TEST(PointerIdentity, FortifiedCopiesPreservePointerIdentity) {
  const auto result = test::analyze(R"c(
void bad(int *p) { int *q;
  __builtin___memcpy_chk(&q, &p, sizeof p, sizeof q);
  free(p); use(q);
}
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
}

TEST(PointerIdentity, APartialPointerCopyReportsLostCoverage) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void partial(int **dest, int **source) { memcpy(dest, source, 1); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"analysis-incomplete"});
  EXPECT_EQ(result.diagnostics.diagnostics()[0].message,
            "analysis is incomplete: unsupported memory copy of "
            "pointer-containing storage");
  EXPECT_FALSE(result.summary("partial")->incomplete.empty());
}

TEST(PointerIdentity, ZeroLengthCopiesDoNotTransferOwnership) {
  const auto result = test::analyze(std::string(Memory) + R"c(
void clean(int *p, int *q) { memcpy(&q, &p, 0); free(p); use(q); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

TEST(PointerIdentity, ArrayDecayPreservesTheElementRecordView) {
  const auto result = test::analyze(R"c(
struct box { int *p; };
static void drop(struct box *b) { free(b->p); }
void bad(int *p) { struct box b[1] = {{p}}; drop(b); use(b[0].p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(PointerIdentity, ConstRecordViewsHaveTheSameLayout) {
  const auto result = test::analyze(R"c(
struct box { int *p; };
static void drop(const struct box *b) { free(b->p); }
void bad(const struct box *b) { drop(b); use(b->p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(test::ids(result.diagnostics),
            std::vector<std::string>{"use-after-free"});
}

TEST(PointerIdentity, UnrelatedRecordViewsDoNotFabricateFieldEffects) {
  const auto result = test::analyze(R"c(
struct first { int *p; }; struct second { int tag; int *p; };
static void drop_field(void *object) { struct first *a = object; free(a->p); }
void boundary(struct second *b) { drop_field(b); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 1U);
}

TEST(PointerIdentity, CompatibleErasedAndEmbeddedRecordViewsRemainChecked) {
  const auto result = test::analyze(R"c(
struct first { int *p; }; struct outer { int tag; struct first in; };
static void drop_field(void *object) { struct first *a = object; free(a->p); }
void bad(struct outer *b) { void *erased = &b->in; drop_field(erased); use(b->in.p); }
)c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countId(result, core::diag::AnalysisIncomplete), 0U);
  EXPECT_EQ(countId(result, core::diag::UseAfterFree), 1U);
}

TEST(PointerIdentity, InvalidCTestFixturesCannotReturnACleanAnalysis) {
  const auto result =
      test::analyze("void broken(void) { missing_declaration(); }");
  EXPECT_FALSE(result.ast);
  EXPECT_FALSE(result.analyzer);
}

} // namespace weavec::analysis
