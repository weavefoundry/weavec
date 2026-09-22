//===- SoundDefaultsTest.cpp - Code the engine cannot see (RFC 0030) ------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §3.1, §5, §6 and §11 (stage S3-B3): the unknown-callee default and
// its replacement by a known release, ownership contracts, inline assembly,
// summaries that carry the default, `may-alias-released`, budgets,
// assumptions, unsafe regions, `setjmp` and `-fno-weavec-zero-init`.
//
//===----------------------------------------------------------------------===//

#include "SiteTestUtils.h"
#include "weavec/Analysis/UnitPipeline.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace weavec::analysis {

using test::collectUnit;
using Lines = std::vector<std::string>;

/// `<text> <facet>=<outcome>[/<reason>][:<template>]...` for every site of
/// `function`.
static Lines outcomes(const core::Ledger &ledger, llvm::StringRef function) {
  Lines out;
  for (const core::FunctionLedger &row : ledger.units.front().functions) {
    if (row.name != function)
      continue;
    for (const core::Site &site : row.sites) {
      std::string line = site.text;
      for (const core::Facet facet : core::AllFacets) {
        const core::FacetRecord *record = site.facet(facet);
        if (record == nullptr)
          continue;
        line += " " + std::string(core::toString(facet)) + "=" +
                record->decision.compact();
        if (record->check)
          line += ":" + std::string(core::toString(record->check->kind));
      }
      out.push_back(line);
    }
  }
  return out;
}

namespace {
struct Piped {
  Lines diagnostics;
  core::Ledger ledger;
};
} // namespace

static Piped pipe(const test::CollectedUnit &unit,
                  const UnitPipelineOptions &options = {}) {
  core::DiagnosticCollector collected;
  const UnitPipelineResult result =
      runUnitAnalysis(unit.context(), options, collected);
  Piped out;
  for (const core::Diagnostic &d : collected.diagnostics()) {
    std::string line = std::to_string(d.location.line) + ": " +
                       std::string(core::toString(d.severity)) + ": " +
                       d.message;
    for (const core::Diagnostic &note : d.notes)
      line += " [" + note.message + "]";
    out.diagnostics.push_back(line);
  }
  if (result.ledger)
    out.ledger = result.ledger->ledger;
  return out;
}

/// The `outcomes` line of the `occurrence`-th site whose text is `text`.
static std::string row(const Piped &piped, llvm::StringRef function,
                       llvm::StringRef text, unsigned occurrence = 0) {
  for (const std::string &line : outcomes(piped.ledger, function))
    if (llvm::StringRef(line).starts_with((text + " ").str()) &&
        occurrence-- == 0)
      return line;
  return {};
}

/// The site of `function` whose text is `text`.
static const core::Site *site(const Piped &piped, llvm::StringRef function,
                              llvm::StringRef text) {
  for (const core::FunctionLedger &row : piped.ledger.units.front().functions)
    if (row.name == function)
      for (const core::Site &candidate : row.sites)
        if (candidate.text == text)
          return &candidate;
  return nullptr;
}

namespace {

// -- §5.1 unknown callees --------------------------------------------------

TEST(SoundDefaults, UnknownCalleeMarksWhatItWasHanded) {
  const auto unit = collectUnit(R"c(
void consume(char *p);
int f(void) { char *p = malloc(8); if (!p) return 0; consume(p); return p[0]; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty()) << piped.diagnostics.front();
  EXPECT_EQ(row(piped, "f", "consume(p)"),
            "consume(p) temporal=unresolved/unknown-callee");
  EXPECT_EQ(
      row(piped, "f", "p[0]"),
      "p[0] spatial=proven null=proven temporal=unresolved/unknown-callee");
  // The suggestion: the annotation that would make the callee known.
  const core::Site *call = site(piped, "f", "consume(p)");
  ASSERT_NE(call, nullptr);
  const core::FacetRecord *temporal = call->facet(core::Facet::Temporal);
  ASSERT_NE(temporal, nullptr);
  EXPECT_EQ(temporal->decision.detail,
            "declare 'consume' with WEAVEC_BORROWED on 'p' if it neither "
            "keeps nor frees it");
  ASSERT_TRUE(temporal->fixit.has_value());
  EXPECT_EQ(temporal->fixit->insertion, "WEAVEC_BORROWED ");
  EXPECT_EQ(temporal->fixit->location.line, 2U);
}

TEST(SoundDefaults, AKnownReleaseReplacesAnUnknownRecord) {
  const auto unit = collectUnit(R"c(
void consume(char *p);
void f(void) { char *p = malloc(8); if (!p) return; consume(p); free(p); p[0] = 1; }
)c");
  const Piped piped = pipe(unit);
  // §3.1: the release after the unknown callee is itself unresolved, and the
  // use after the release is the definite use-after-free v0.10.0 reports.
  EXPECT_EQ(piped.diagnostics,
            (Lines{"3: error: use of 'p' after it was freed [freed here]"}));
  EXPECT_TRUE(llvm::StringRef(row(piped, "f", "free(p)"))
                  .ends_with("temporal=unresolved/unknown-callee"))
      << row(piped, "f", "free(p)");
  EXPECT_TRUE(
      llvm::StringRef(row(piped, "f", "p[0]")).ends_with("temporal=violation"))
      << row(piped, "f", "p[0]");
}

TEST(SoundDefaults, OnlyOwnershipContractsLiftTheDefault) {
  const auto unit = collectUnit(R"c(
void inspect(const char *__attribute__((annotate("weavec.borrowed"))) p);
void destroy(char *p) __attribute__((nonnull));
int f(void) {
  char *p = malloc(8); if (!p) return 0;
  inspect(p); int v = p[0];
  destroy(p); return v + p[1];
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty()) << piped.diagnostics.front();
  EXPECT_EQ(row(piped, "f", "inspect(p)"),
            "inspect(p) temporal=trusted/extern-contract");
  EXPECT_TRUE(
      llvm::StringRef(row(piped, "f", "p[0]")).ends_with("temporal=proven"));
  // `nonnull` is no ownership contract.
  EXPECT_TRUE(llvm::StringRef(row(piped, "f", "destroy(p)"))
                  .contains("temporal=unresolved/unknown-callee"))
      << row(piped, "f", "destroy(p)");
  EXPECT_TRUE(llvm::StringRef(row(piped, "f", "p[1]"))
                  .ends_with("temporal=unresolved/unknown-callee"));
}

TEST(SoundDefaults, UnknownCodeForgetsWhatItReaches) {
  // The argument's own value keeps its nullness and extent; what the callee
  // could write through it is unknown (the per-unit false proof of
  // WholeProgram-rfc0013-input-identity).
  const auto unit = collectUnit(R"c(
struct box { char *data; };
void swap(struct box *a);
void f(void) {
  struct box b = {malloc(8)}; if (!b.data) return;
  b.data[4] = 1;
  swap(&b);
  b.data[4] = 2;
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty()) << piped.diagnostics.front();
  EXPECT_TRUE(llvm::StringRef(row(piped, "f", "b.data[4]", 0))
                  .starts_with("b.data[4] spatial=proven"));
  EXPECT_EQ(row(piped, "f", "b.data[4]", 1),
            "b.data[4] spatial=unresolved/unknown-extent null=checked:nonnull "
            "temporal=unresolved/unknown-callee");
}

TEST(SoundDefaults, ThirdPartySystemHeadersAreUnknownCode) {
  // §5.2: only the platform's own headers are trusted.
  const auto unit = collectUnit(R"c(
#include <sys.h>
int f(void) { char *p = malloc(8); if (!p) return 0; lib_touch(p); return p[0]; }
)c",
                                "void lib_touch(char *p);\n");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(llvm::StringRef(row(piped, "f", "lib_touch(p)"))
                  .contains("unresolved/unknown-callee"))
      << row(piped, "f", "lib_touch(p)");
  // No fix-it into a system header.
  const core::Site *call = site(piped, "f", "lib_touch(p)");
  ASSERT_NE(call, nullptr);
  EXPECT_FALSE(call->facet(core::Facet::Temporal)->fixit.has_value());
}

TEST(SoundDefaults, InlineAssemblyIsUnknownCode) {
  // §5.7.
  const auto unit = collectUnit(R"c(
int f(void) {
  char *p = malloc(8); if (!p) return 0;
  __asm__ volatile("" : : "r"(p) : "memory");
  return p[0];
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(
      row(piped, "f", "p[0]"),
      "p[0] spatial=proven null=proven temporal=unresolved/unknown-callee");
}

TEST(SoundDefaults, SummariesCarryTheDefaultToCallers) {
  // A wrapper handing its parameter to unknown code: its summary says so
  // (`unknown`), and its callers apply the default.
  const auto unit = collectUnit(R"c(
void consume(char *p);
static void wrap(char *p) { consume(p); }
int f(void) { char *p = malloc(8); if (!p) return 0; wrap(p); return p[0]; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty()) << piped.diagnostics.front();
  EXPECT_EQ(row(piped, "f", "wrap(p)"), "wrap(p) temporal=proven");
  EXPECT_EQ(
      row(piped, "f", "p[0]"),
      "p[0] spatial=proven null=proven temporal=unresolved/unknown-callee");
}

TEST(SoundDefaults, ACopyKeepsTheCertaintyOfItsRecord) {
  // §3.1: a copy names the object its source names, with the source's
  // record: a copy of an unknown-origin or possible record is not definite
  // (`walk` is cJSON_Delete's loop).
  const auto unit = collectUnit(R"c(
void drop(void *p);
void unknown(char *p) { drop(p); char *q = p; q[0] = 1; }
void possible(int c, char *p) { if (c) free(p); char *q = p; q[0] = 1; }
typedef struct node { struct node *next; } node;
void walk(node *item) {
  node *next = 0;
  while (item != 0) { next = item->next; drop(item); item = next; }
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"4: warning: use of 'p' after it may have been freed "
                   "[freed here on some paths]",
                   "4: warning: use of 'q' after it may have been freed "
                   "[freed here on some paths (through 'p')]"}));
  EXPECT_EQ(row(piped, "unknown", "q[0]"),
            "q[0] spatial=proven null=checked:nonnull "
            "temporal=unresolved/unknown-callee");
}

TEST(SoundDefaults, OutcomeClassesCarryTheDefaultAsUnknown) {
  // A callee that hands memory to unknown code on the class the caller
  // selects: the class says `unknown`, not a release (so the test of the
  // result settles nothing definite), and a store the unknown code may have
  // kept is not claimed missing on that class, which would make `b.data`
  // null there (cJSON's `print_value` in its callback context).
  const auto unit = collectUnit(R"c(
struct box { char *data; };
void drop(void *p);
void keep(struct box *b);
static int give(struct box *b) { if (!b->data) return 0; drop(b->data); return 1; }
static int grow(struct box *b) {
  char *n = malloc(8);
  if (!n) { free(b->data); b->data = 0; return 0; }
  free(b->data);
  b->data = n;
  keep(b);
  return 1;
}
void f(void) {
  struct box b;
  b.data = malloc(4);
  if (!b.data || !give(&b)) return;
  b.data[0] = 1;
}
void g(void) {
  struct box b;
  b.data = malloc(4);
  if (!b.data || !grow(&b)) return;
  b.data[1] = 1;
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty()) << piped.diagnostics.front();
  EXPECT_TRUE(llvm::StringRef(row(piped, "f", "b.data[0]"))
                  .ends_with("temporal=unresolved/unknown-callee"))
      << row(piped, "f", "b.data[0]");
  EXPECT_TRUE(llvm::StringRef(row(piped, "g", "b.data[1]"))
                  .ends_with("temporal=unresolved/unknown-callee"))
      << row(piped, "g", "b.data[1]");
}

// -- §3.1 aliases of a released object -------------------------------------

TEST(SoundDefaults, APointerThatMayAliasAReleasedObjectIsUnresolved) {
  const char *code = R"c(
void two(char *a, char *b) { free(a); b[0] = 1; }
void typed(int *x, long *y) { free(x); y[0] = 1; }
void fresh(char *a, char **out) { free(a); *out = malloc(4); if (*out) (*out)[0] = 1; }
struct buf { char *data; char *cur; };
void cursor(struct buf *b) { free(b->data); b->cur[0] = 0; }
)c";
  const auto unit = collectUnit(code);
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty()) << piped.diagnostics.front();
  EXPECT_TRUE(llvm::StringRef(row(piped, "two", "b[0]"))
                  .ends_with("temporal=unresolved/may-alias-released"));
  EXPECT_TRUE(llvm::StringRef(row(piped, "cursor", "b->cur[0]"))
                  .ends_with("temporal=unresolved/may-alias-released"));
  // Incompatible non-character types, and a value stored after the release.
  EXPECT_TRUE(
      llvm::StringRef(row(piped, "typed", "y[0]")).ends_with("temporal=proven"))
      << row(piped, "typed", "y[0]");
  EXPECT_TRUE(llvm::StringRef(row(piped, "fresh", "(*out)[0]"))
                  .ends_with("temporal=proven"))
      << row(piped, "fresh", "(*out)[0]");
  // `-fno-strict-aliasing`: every two pointee types may designate one object.
  UnitPipelineOptions relaxed;
  relaxed.engine.strictAliasing = false;
  const Piped loose = pipe(unit, relaxed);
  EXPECT_TRUE(llvm::StringRef(row(loose, "typed", "y[0]"))
                  .ends_with("temporal=unresolved/may-alias-released"));
}

// -- §5.5 budgets ----------------------------------------------------------

TEST(SoundDefaults, AFunctionOverItsBudgetTakesTheDefaults) {
  const auto unit = collectUnit(R"c(
int walk(const int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) s += p[i];
  return s;
}
)c");
  UnitPipelineOptions tight;
  tight.engine.budget = 1;
  const Piped piped = pipe(unit, tight);
  ASSERT_EQ(piped.ledger.units.front().functions.size(), 1U);
  EXPECT_TRUE(piped.ledger.units.front().functions.front().overBudget);
  EXPECT_EQ(row(piped, "walk", "p[i]"),
            "p[i] spatial=unresolved/budget null=checked:nonnull "
            "temporal=unresolved/budget");
  // Unlimited, the same function is analysed.
  UnitPipelineOptions unlimited;
  unlimited.engine.budget = 0;
  const Piped full = pipe(unit, unlimited);
  EXPECT_FALSE(full.ledger.units.front().functions.front().overBudget);
  EXPECT_TRUE(
      llvm::StringRef(row(full, "walk", "p[i]")).ends_with("temporal=proven"));
}

// -- §6.2 assumptions ------------------------------------------------------

TEST(SoundDefaults, AssumptionsAreProvenCheckedOrContradicted) {
  const auto unit = collectUnit(R"c(
int proven(int n) { if (n <= 0) return 0; ASSUME(n > 0); return n; }
int checked(int n) { ASSUME(n > 0); return n; }
int refuted(void) { int i = 10; ASSUME(i < 4); return i; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"4: error: assumption 'i < 4' is false here ['i' is 10 "
                   "here]"}));
  EXPECT_EQ(row(piped, "proven", "ASSUME(n>0)"),
            "ASSUME(n>0) assertion=proven");
  EXPECT_EQ(row(piped, "checked", "ASSUME(n>0)"),
            "ASSUME(n>0) assertion=checked:assert");
  EXPECT_EQ(row(piped, "refuted", "ASSUME(i<4)"),
            "ASSUME(i<4) assertion=violation");
}

// -- §6.1 unsafe regions ---------------------------------------------------

TEST(SoundDefaults, UnsafeRegionsTrustRefineNothingAndDropNoDiagnostic) {
  const auto unit = collectUnit(R"c(
int f(int *p, int i) { int a; UNSAFE { a = p[i]; } return a + p[0]; }
void g(char *p) { UNSAFE { free(p); p[0] = 1; } }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"3: error: use of 'p' after it was freed [freed here]"}));
  EXPECT_EQ(row(piped, "f", "p[i]"),
            "p[i] spatial=trusted/unsafe null=trusted/unsafe temporal=proven");
  // The trusted dereference did not make `p` non-null (its Single default
  // proves the element, §7.3).
  EXPECT_EQ(row(piped, "f", "p[0]"),
            "p[0] spatial=proven null=checked:nonnull temporal=proven");
}

// -- §5.4 setjmp and §11 zero-initialisation ---------------------------------

TEST(SoundDefaults, SetjmpMakesTemporalFacetsUnresolved) {
  const auto unit = collectUnit(R"c(
static jmp_buf env;
int run(char *p) {
  if (p == 0) return -1;
  if (setjmp(env) != 0) return p[0];
  return 0;
}
)c");
  const Piped piped = pipe(unit);
  // §5.4: what the flow proved (here `p`'s Single default) may be stale
  // after a `longjmp`.
  EXPECT_EQ(row(piped, "run", "p[0]"),
            "p[0] spatial=unresolved/setjmp null=checked:nonnull "
            "temporal=unresolved/setjmp");
}

TEST(SoundDefaults, WithoutZeroInitialisationAMaybeUninitialisedPointer) {
  const auto unit = collectUnit(R"c(
int deref(int c, int *q) { int *p; if (c) p = q; return *p; }
)c");
  EXPECT_TRUE(llvm::StringRef(row(pipe(unit), "deref", "*p"))
                  .contains("null=checked:nonnull"));
  UnitPipelineOptions none;
  none.engine.zeroInit = false;
  EXPECT_TRUE(llvm::StringRef(row(pipe(unit, none), "deref", "*p"))
                  .contains("null=unresolved/no-zero-init"));
}

} // namespace
} // namespace weavec::analysis
