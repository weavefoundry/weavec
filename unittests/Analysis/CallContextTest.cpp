//===- CallContextTest.cpp - RFC 0016 compositional call checking ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Frontend/Sidecar.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace weavec::analysis {

static std::size_t countContextDiagnostic(const test::AnalysisResult &result,
                                          std::string_view id) {
  return static_cast<std::size_t>(
      std::ranges::count_if(result.diagnostics.diagnostics(),
                            [id](const core::Diagnostic &diagnostic) {
                              return diagnostic.id == id;
                            }));
}

static void expectCleanContext(const std::string &code) {
  const auto result = test::analyze(code);
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

static void expectContextError(const std::string &code, std::string_view id) {
  const auto result = test::analyze(code);
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countContextDiagnostic(result, id), 0U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
  EXPECT_EQ(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U)
      << ::testing::PrintToString(test::messages(result.diagnostics));
}

static const std::string Entry = R"c(
void test(void) { char *p = malloc(4); if (!p) return;
)c";

TEST(CompositionalCall, ReleaseBeforeReadAndWriteReportAtTheCallee) {
  for (const auto *operation : {"*b = 1;", "return *b;"}) {
    const auto result = test::analyze(
        std::string("static int zap(char *a, char *b) { free(a); ") +
        operation + " return 0; }" + Entry + "zap(p, p); }");
    ASSERT_TRUE(result.ast);
    EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
    ASSERT_EQ(result.diagnostics.size(), 1U);
    EXPECT_EQ(result.diagnostics.diagnostics()[0].location.line, 1U);
    EXPECT_FALSE(result.diagnostics.diagnostics()[0].notes.empty());
  }
}

TEST(CompositionalCall, ReadingOrWritingBeforeReleaseRemainsClean) {
  expectCleanContext(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p); }");
  expectCleanContext(R"c(
static int zap(char *a, char *b) { int value = *b; free(a); return value; }
)c" + Entry + "(void)zap(p, p); }");
}

TEST(CompositionalCall, TwoSourceReleasesDifferFromTwoPathsOfOneRelease) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); free(b); }
)c" + Entry + "zap(p, p); }",
                     core::diag::DoubleFree);
  expectCleanContext(R"c(
static void zap(char *a, char *b) {
  char *saved = a;
  if (saved == b) free(saved);
  else { free(a); free(b); }
}
)c" + Entry + "zap(p, p); }");
}

TEST(CompositionalCall, IndependentAllocationsKeepIndependentLifetimes) {
  expectCleanContext(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
zap(p, q); free(q); }
)c");
}

TEST(CompositionalCall, AliasedOutputStorageCarriesOrderAndFinalValues) {
  expectContextError(R"c(
static void zap(char **a, char **b) { free(*a); **b = 1; }
)c" + Entry + "zap(&p, &p); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char **a, char **b) { free(*a); *a = 0; if (*b) **b = 1; }
)c" + Entry + "zap(&p, &p); free(p); }");
  expectCleanContext(R"c(
static void zap(char **a, char **b) {
  free(*a); *a = malloc(4); if (*b) **b = 1;
}
)c" + Entry + "zap(&p, &p); if (p) *p = 2; free(p); }");
}

TEST(CompositionalCall, ReplacingTheCellDoesNotReviveASavedInput) {
  expectContextError(R"c(
static void zap(char **a, char **b) {
  char *saved = *b; free(*a); *a = malloc(4); *saved = 1;
}
)c" + Entry + "zap(&p, &p); free(p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, DifferentOutputCellsMayContainTheSameChild) {
  expectContextError(R"c(
static void zap(char **a, char **b) { free(*a); *a = 0; if (*b) **b = 1; }
)c" + Entry + "char *q = p; zap(&p, &q); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char **a, char **b) { free(*a); *a = 0; if (*b) **b = 1; }
)c" + Entry + "char *q = malloc(4); zap(&p, &q); free(q); }");
}

TEST(CompositionalCall, PointerValueAndAddressOfItsCellAreDistinct) {
  expectCleanContext(R"c(
static void zap(char **out, char *value) { *out = 0; free(value); }
)c" + Entry + "zap(&p, p); free(p); }");
}

TEST(CompositionalCall, RecordChildrenRetainSharedIdentity) {
  expectContextError(R"c(
struct Box { char *data; };
static void zap(struct Box *a, struct Box *b) { free(a->data); free(b->data); }
)c" + Entry + "struct Box a = {p}, b = {p}; zap(&a, &b); }",
                     core::diag::DoubleFree);
}

TEST(CompositionalCall, EntryScalarFactsPruneOnlyTheirFeasibleBranch) {
  expectCleanContext(R"c(
static void zap(char *a, char *b, int release) {
  if (release) free(a); else { *b = 1; free(a); }
}
)c" + Entry + "zap(p, p, 0); }");
  expectContextError(R"c(
static void zap(char *a, char *b, int release) { if (release) free(a); *b = 1; }
)c" + Entry + "zap(p, p, 1); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, EntryFieldFactsAreNotLostWhenAliasesAreInstalled) {
  expectCleanContext(R"c(
struct Box { char *data; int release; };
static void zap(struct Box *a, char *b) {
  if (a->release) free(a->data); else *b = 1;
}
)c" + Entry + "struct Box a = {p, 0}; zap(&a, p); free(p); }");
}

TEST(CompositionalCall,
     InteriorPointersKeepTemporalIdentityAndRelativeOffsets) {
  const auto result = test::analyze(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p + 1); }");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::InvalidRelease), 0U);
  expectCleanContext(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p + 1); }");
}

TEST(CompositionalCall, ForwardingKeepsNestedDiagnosticsAndCallNotes) {
  const auto result = test::analyze(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
static void middle(char *a, char *b) { zap(a, b); }
static void outer(char *a, char *b) { middle(a, b); }
)c" + Entry + "outer(p, p); }");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U);
}

TEST(CompositionalCall, CallbackTargetsAndDataAliasesSelectOneContext) {
  expectContextError(R"c(
static void drop(char *p) { free(p); }
static void zap(void (*fn)(char *), char *a, char *b) { fn(a); *b = 1; }
)c" + Entry + "zap(drop, p, p); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void drop(char *p) { free(p); }
static void zap(void (*fn)(char *), char *a, char *b) { *b = 1; fn(a); }
)c" + Entry + "zap(drop, p, p); }");
}

TEST(CompositionalCall, KnownIndirectTargetsUseTheSameArgumentContext) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "void (*fn)(char *, char *) = zap; fn(p, p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, GlobalAndParameterInputsCanShareAnAllocation) {
  expectContextError(R"c(
char *global;
static void zap(char *a) { free(a); *global = 1; }
)c" + Entry + "global = p; zap(p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, SelectedCellsKeepTheirPointeeIdentities) {
  expectContextError(R"c(
static void zap(char **a, int i, int j) { free(a[i]); *a[j] = 1; }
)c" + Entry + "char *items[2] = {p, p}; zap(items, 0, 1); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char **a, int i, int j) { free(a[i]); *a[j] = 1; }
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
char *items[2] = {p, q}; zap(items, 0, 1); free(q); }
)c");
}

TEST(CompositionalCall, UnsafeRequestsRetainEffectsWithoutDelayedReports) {
  expectCleanContext(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "UNSAFE { zap(p, p); } }");
  expectCleanContext(R"c(
static void UNSAFE zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p); }");
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "UNSAFE { zap(p, p); } *p = 2; }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, UnsafeAndSafeRequestsDoNotShareReportingState) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
static void unchecked(char *p) { UNSAFE { zap(p, p); } }
)c" + Entry + "zap(p, p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, BoundedRecursiveContextsReachTheBaseCase) {
  expectContextError(R"c(
static void zap(char *a, char *b, int n) {
  if (n) zap(a, b, n - 1); else { free(a); *b = 1; }
}
)c" + Entry + "zap(p, p, 2); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, AnUnconditionalBodyErrorSurvivesContextSelection) {
  expectContextError(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); free(a); }
)c" + Entry + "zap(p, p); }",
                     core::diag::DoubleFree);
}

static const std::string Shares = R"c(
struct Object { unsigned refs; int value; };
static struct Object *retain(struct Object *p) { p->refs++; return p; }
static void drop(struct Object *p) { if (--p->refs == 0) free(p); }
static void zap(struct Object *a, struct Object *b) { drop(a); drop(b); }
void test(void) {
  struct Object *p = malloc(sizeof *p); if (!p) return; p->refs = 1;
)c";

TEST(CompositionalCall, SameShareAndSeparatelyRetainedSharesDiffer) {
  expectContextError(Shares + "zap(p, p); }", core::diag::DoubleFree);
  expectCleanContext(Shares + "struct Object *q = retain(p); zap(p, q); }");
}

TEST(CompositionalCall, ReanalysisInvalidatesTheResultsOfChangedCallees) {
  auto result = test::analyze(R"c(
static void zap(char *a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p); }");
  ASSERT_TRUE(result.ast);
  auto &store = result.analyzer->summaries();
  EXPECT_FALSE(store.memorySpecialized.empty());
  core::FunctionSummary changed;
  changed.neverReturns = true;
  ASSERT_TRUE(result.function("zap"));
  store.setInferred(*result.function("zap"), changed);
  EXPECT_TRUE(store.memorySpecialized.empty());
  EXPECT_TRUE(store.memoryDiagnostics.empty());
  EXPECT_FALSE(store.memoryRequests.empty());
}

TEST(CompositionalCall, ExportedRequestsAndResultsSurviveSidecars) {
  auto result = test::analyze(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p); }");
  ASSERT_TRUE(result.ast);
  frontend::UnitRecord record;
  record.exports = result.analyzer->exports();
  ASSERT_TRUE(record.exports.functions.contains("zap"));
  EXPECT_FALSE(
      record.exports.functions.at("zap").memorySpecializations.empty());
  const auto parsed =
      frontend::parseUnitRecord(frontend::printUnitRecord(record));
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(record.exports.sameSummariesAs(parsed->exports));
}

TEST(CompositionalCall, WritesThroughAliasesInvalidateEntryFieldFacts) {
  expectContextError(R"c(
struct Box { char *data; int release; };
static void zap(struct Box *a, struct Box *b, char *p) {
  b->release = 1;
  if (a->release) free(a->data);
  *p = 1;
}
)c" + Entry + "struct Box a = {p, 0}; zap(&a, &a, p); }",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, AChangedScalarAtOneCallSiteSelectsANewContext) {
  expectContextError(R"c(
static void zap(char *a, char *b, int release) { if (release) free(a); *b = 1; }
void test(void) {
  for (int i = 0; i < 2; ++i) {
    char *p = malloc(4); if (!p) return;
    zap(p, p, i);
    if (!i) free(p);
  }
}
)c",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, RecursiveAndDepthLimitsRemainExplicit) {
  const auto result = test::analyze(R"c(
static void zap(char *a, char *b, int n) {
  if (n) zap(a, b, n - 1); else { free(a); *b = 1; }
}
)c" + Entry + "zap(p, p, 20); }");
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U);
  EXPECT_TRUE(std::ranges::any_of(
      result.diagnostics.diagnostics(), [](const core::Diagnostic &diagnostic) {
        return diagnostic.message == "analysis is incomplete: call context "
                                     "unavailable or limit reached";
      }));
}

TEST(CompositionalCall, TooManyDistinctContextsRetainGenericEffects) {
  std::string code = R"c(
static void zap(char *a, char *b, int selector) {
  if (selector < 0) free(a); *b = 1;
}
void test(void) {
)c";
  for (unsigned i = 0; i < core::MaxMemoryContexts + 1; ++i)
    code += "{ char *p = malloc(4); if (!p) return; zap(p, p, " +
            std::to_string(i) + "); free(p); }\n";
  const auto result = test::analyze(code + "}");
  ASSERT_TRUE(result.ast);
  EXPECT_GT(countContextDiagnostic(result, core::diag::AnalysisIncomplete), 0U);
  for (const auto &[symbol, requests] :
       result.analyzer->summaries().memoryRequests) {
    (void)symbol;
    EXPECT_LE(requests.size(), core::MaxMemoryContexts);
  }
}

TEST(CompositionalCall, OversizedInputFootprintsHaveAnExplicitBoundary) {
  std::string code = "static void zap(";
  for (unsigned i = 0; i < core::MaxCallContextPaths + 1; ++i)
    code += (i ? ", char *p" : "char *p") + std::to_string(i);
  code += ") { free(p0);";
  for (unsigned i = 1; i < core::MaxCallContextPaths + 1; ++i)
    code += "*p" + std::to_string(i) + " = 1;";
  code += "}" + Entry + "zap(p";
  for (unsigned i = 1; i < core::MaxCallContextPaths + 1; ++i)
    code += ", p";
  const auto result = test::analyze(code + ");}");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(std::ranges::any_of(
      result.diagnostics.diagnostics(), [](const core::Diagnostic &diagnostic) {
        return diagnostic.message ==
               "analysis is incomplete: call context input path limit reached";
      }));
}

TEST(CompositionalCall, InvalidTypedContextsAreNeverCachedAsChecked) {
  auto result = test::analyze("void zap(int value) { (void)value; }");
  ASSERT_TRUE(result.ast);
  core::CallContext input;
  input.facts[core::SummaryPath::param(0)] =
      core::ValueFact::of(core::Outcome::NonNull);
  auto &store = result.analyzer->summaries();
  EXPECT_FALSE(store.specializeMemory("zap", input, {}));
  EXPECT_TRUE(store.memorySpecialized.empty());
}

TEST(CompositionalCall, OwnershipAnnotationsStillCheckKnownBodiesInContext) {
  expectContextError(R"c(
static void zap(char *OWNED a, char *b) { free(a); *b = 1; }
)c" + Entry + "zap(p, p); }",
                     core::diag::UseAfterFree);
  expectCleanContext(R"c(
static void zap(char *OWNED a, char *b) { *b = 1; free(a); }
)c" + Entry + "zap(p, p); }");
}

TEST(CompositionalCall, RewrittenSelectedCellsDoNotKeepObjectSeparation) {
  expectContextError(R"c(
static void zap(char **a, int i, int j) { free(a[i]); *a[j] = 1; }
static void middle(char **a, int i, int j) {
  free(a[j]); a[j] = a[i]; zap(a, i, j);
}
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
char *items[2] = {p, q}; middle(items, 0, 1); }
)c",
                     core::diag::UseAfterFree);
}

TEST(CompositionalCall, PointerReassignmentAtOneCallSiteRechecksTheContext) {
  expectContextError(R"c(
static void zap(char *a, char *b) { free(a); *b = 1; }
)c" + Entry + R"c(
char *q = malloc(4); if (!q) { free(p); return; }
char *a = p;
for (int i = 0; i < 2; ++i) { zap(a, q); a = q; }
}
)c",
                     core::diag::UseAfterFree);
}

TEST(CompositionalDatabase, ContextGlobalsAreRenumberedWithTheirSummaries) {
  UnitExports first;
  first.source = "first.c";
  (void)first.globals.idFor("unrelated");
  UnitExports second;
  second.source = "second.c";
  const auto global = core::SummaryPath::global(second.globals.idFor("shared"));
  core::CallContext input;
  ASSERT_TRUE(input.addAlias(
      {.first = core::SummaryPath::param(0), .second = global, .offset = {}}));
  core::FunctionSummary summary;
  summary.addEffect(global, {.freed = true});
  second.functions["zap"].memorySpecializations[input] = summary;
  second.memoryRequests["zap"].insert(input);
  ProgramDatabase db;
  db.add(first);
  db.add(second);
  const auto &requests = db.memoryRequestsFor("zap");
  ASSERT_EQ(requests.size(), 1U);
  const auto *found = db.findMemorySpecialization("zap", *requests.begin());
  ASSERT_NE(found, nullptr);
  const auto remapped = requests.begin()->aliases.begin()->second;
  EXPECT_NE(remapped, global);
  EXPECT_TRUE(found->effectOf(remapped).freed);
  EXPECT_FALSE(found->effectOf(global).freed);
}

TEST(CompositionalDatabase, DuplicateDefinitionsJoinContextEffects) {
  UnitExports first;
  UnitExports second;
  core::CallContext input;
  ASSERT_TRUE(input.addAlias({.first = core::SummaryPath::param(0),
                              .second = core::SummaryPath::param(1),
                              .offset = {}}));
  first.functions["zap"].memorySpecializations[input].addEffect(
      core::SummaryPath::param(0), {.freed = true});
  second.functions["zap"].memorySpecializations[input].addEffect(
      core::SummaryPath::param(1).deref(), {.written = true});
  ProgramDatabase db;
  db.add(first);
  db.add(second);
  const auto *found = db.findMemorySpecialization("zap", input);
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->frees(0));
  EXPECT_TRUE(found->effectOf(core::SummaryPath::param(1).deref()).written);
}

TEST(CompositionalDatabase, MissingGlobalRejectsTheEntireContext) {
  UnitExports unit;
  core::CallContext input;
  ASSERT_TRUE(input.addAlias({.first = core::SummaryPath::param(0),
                              .second = core::SummaryPath::global(4),
                              .offset = {}}));
  unit.memoryRequests["zap"].insert(input);
  unit.functions["zap"].memorySpecializations[input].addEffect(
      core::SummaryPath::param(0), {.freed = true});
  ProgramDatabase db;
  db.add(unit);
  EXPECT_TRUE(db.memoryRequestsFor("zap").empty());
  EXPECT_EQ(db.findMemorySpecialization("zap", input), nullptr);
}

TEST(CompositionalDatabase, InvalidTypedRequestRetainsOrdinaryBodyErrors) {
  UnitExports unit;
  core::CallContext input;
  input.facts[core::SummaryPath::param(0)] =
      core::ValueFact::of(core::Outcome::NonNull);
  unit.memoryRequests["zap"].insert(input);
  ProgramDatabase db;
  db.add(unit);
  const auto result = test::analyzeInProgram(R"c(
void zap(int value) {
  char *p = malloc(4); if (!p) return;
  free(p); *p = 1;
}
)c",
                                             &db);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(countContextDiagnostic(result, core::diag::UseAfterFree), 1U);
}

TEST(CompositionalCall, CopiedBorrowOffsetsCannotProveDifferentAddressesEqual) {
  for (const auto *storage :
       {"char data[2]; char *a = data; char *b = data + 1;",
        "struct Pair { char first; char second; } data; "
        "char *a = (char *)&data; char *b = &data.second;"}) {
    expectContextError(R"c(
static void zap(char *a, char *b, char *p) {
  if (a != b) free(p); *p = 1;
}
)c" + Entry + storage + "zap(a, b, p); }",
                       core::diag::UseAfterFree);
  }
}

} // namespace weavec::analysis
