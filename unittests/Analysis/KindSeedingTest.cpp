//===- KindSeedingTest.cpp - Pointer kinds in the engine (RFC 0030) -------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §15 item 14 and §7.3–§7.5 (stage S6): the extents the unit's kinds
// seed at parameter entry, at slot loads and at call results, and the
// Call-site records and body decisions of must-access requirements.
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

namespace {

struct Piped {
  Lines diagnostics;
  core::Ledger ledger;
};

/// The unit through the whole pipeline, as `weavec` runs it.
Piped pipe(const test::CollectedUnit &unit) {
  core::DiagnosticCollector collected;
  const UnitPipelineResult result =
      runUnitAnalysis(unit.context(), UnitPipelineOptions{}, collected);
  Piped out;
  for (const core::Diagnostic &d : collected.diagnostics())
    out.diagnostics.push_back(std::to_string(d.location.line) + ": " +
                              d.message);
  if (result.ledger)
    out.ledger = result.ledger->ledger;
  return out;
}

/// `<outcome>[/<reason>][:<template>]`.
std::string spell(const core::FacetDecision &decision,
                  const std::optional<core::FacetCheck> &check) {
  std::string text = decision.compact();
  if (check)
    text += ":" + std::string(core::toString(check->kind));
  return text;
}

/// The decision of `facet` at the site of `function` whose text is `text`
/// (the first one), or "".
std::string facetOf(const Piped &piped, llvm::StringRef function,
                    llvm::StringRef text, core::Facet facet) {
  for (const core::FunctionLedger &row : piped.ledger.units.front().functions) {
    if (row.name != function)
      continue;
    for (const core::Site &site : row.sites) {
      if (site.text != text)
        continue;
      if (const core::FacetRecord *record = site.facet(facet))
        return spell(record->decision, record->check);
    }
  }
  return {};
}

// -- §15 item 14: seeding ------------------------------------------------

TEST(KindSeeding, ASingleParameterCoversOneElement) {
  const auto unit = collectUnit(R"c(
struct s { int a; int b; };
int f(struct s *p) { return p->b + (*p).a; }
int g(int *p) { return p[0]; }
int h(int *p, int c) { return c ? p[1] : 0; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(facetOf(piped, "f", "p->b", core::Facet::Spatial), "proven");
  EXPECT_EQ(facetOf(piped, "g", "p[0]", core::Facet::Spatial), "proven");
  // §7.1: a lower bound decides only what it covers (`p[1]` is under a
  // branch, so no §7.5 requirement covers it either).
  EXPECT_EQ(facetOf(piped, "h", "p[1]", core::Facet::Spatial),
            "unresolved/unknown-extent");
}

TEST(KindSeeding, SlotsAndResultsCarryTheirKinds) {
  const auto unit = collectUnit(R"c(
struct node { struct node *next; int v; };
struct node *lookup(int key);
int second(struct node *n) { return n->next->v; }
int found(int key) { struct node *n = lookup(key); return n ? n->v : 0; }
int direct(int key) { return lookup(key)->v; }
)c");
  const Piped piped = pipe(unit);
  // The slot `node.next` stays Single; `lookup`'s result is Single by A3.
  EXPECT_EQ(facetOf(piped, "second", "n->next->v", core::Facet::Spatial),
            "proven");
  EXPECT_EQ(facetOf(piped, "found", "n->v", core::Facet::Spatial), "proven");
  EXPECT_EQ(facetOf(piped, "direct", "lookup(key)->v", core::Facet::Spatial),
            "proven");
}

TEST(KindSeeding, ACursorStoredDemotesItsSlot) {
  const auto unit = collectUnit(R"c(
struct scan { char *pos; };
void step(struct scan *s) { s->pos = s->pos + 1; }
int peek(struct scan *s) { return s->pos[0]; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(facetOf(piped, "peek", "s->pos[0]", core::Facet::Spatial),
            "unresolved/unknown-extent");
}

TEST(KindSeeding, ArgvIsCountedAndItsStringsTrusted) {
  const auto unit = collectUnit(R"c(
int atoi(const char *);
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  return atoi(argv[1]) + argv[1][0];
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(facetOf(piped, "main", "argv[1]", core::Facet::Spatial), "proven");
  EXPECT_EQ(facetOf(piped, "main", "atoi(argv[1])", core::Facet::Spatial),
            "trusted/system-api");
  EXPECT_EQ(facetOf(piped, "main", "argv[1][0]", core::Facet::Spatial),
            "trusted/system-api");
}

// -- §7.5: must-access requirements -------------------------------------

TEST(KindSeeding, AStaticCalleeIsCheckedAtItsCalls) {
  const auto unit = collectUnit(R"c(
static void fill(char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    p[i] = 0;
}
static int first(const int *p) { return p[0]; }
void calls(size_t n, int *q) {
  char four[4];
  fill(four, n);
  fill(four, 4);
  fill(four, 5);
  (void)first(q);
  (void)first(0);
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"11: 'fill' requires 5 bytes behind 'four', which has 4 "
                   "bytes",
                   "13: a null pointer is passed to 'first', which "
                   "dereferences it"}));
  // Inside, the requirement holds: the covered accesses are proven.
  EXPECT_EQ(facetOf(piped, "fill", "p[i]", core::Facet::Spatial), "proven");
  EXPECT_EQ(facetOf(piped, "fill", "p[i]", core::Facet::Null), "proven");
  EXPECT_EQ(facetOf(piped, "first", "p[0]", core::Facet::Null), "proven");
  // At the calls: checked under the guard, proven, or the violation.
  EXPECT_EQ(facetOf(piped, "calls", "fill(four,n)", core::Facet::Spatial),
            "checked:len");
  EXPECT_EQ(facetOf(piped, "calls", "fill(four,4)", core::Facet::Spatial),
            "proven");
  EXPECT_EQ(facetOf(piped, "calls", "fill(four,5)", core::Facet::Spatial),
            "violation");
  EXPECT_EQ(facetOf(piped, "calls", "first(q)", core::Facet::Null),
            "checked:nonnull");
}

TEST(KindSeeding, AGuardOverASignedBoundHasNoTerm) {
  const auto unit = collectUnit(R"c(
static int upto(const int *p, int last) {
  int s = 0;
  for (int i = 0; i <= last; i++)
    s += p[i];
  return s;
}
static int under(const int *p, unsigned n) {
  int s = 0;
  for (unsigned i = 0; i < n; i++)
    s += p[i];
  return s;
}
int calls(int k, unsigned m) {
  int four[4] = {0};
  return upto(four, k) + upto(0, -1) + under(four, m);
}
)c");
  const Piped piped = pipe(unit);
  // §10.2: a have stops a negative argument at zero, so `last + 1` would
  // pass for `last == -1`; the guard has no term and the call stays open.
  EXPECT_EQ(facetOf(piped, "calls", "upto(four,k)", core::Facet::Spatial),
            "unresolved/inexpressible");
  // The guard is false here, so the requirement needs nothing.
  EXPECT_EQ(facetOf(piped, "calls", "upto(0,-1)", core::Facet::Spatial),
            "proven");
  EXPECT_EQ(facetOf(piped, "calls", "upto(0,-1)", core::Facet::Null),
            "proven");
  // An unsigned bound is exact: `0 < m` is the term `m`.
  EXPECT_EQ(facetOf(piped, "calls", "under(four,m)", core::Facet::Spatial),
            "checked:len");
}

TEST(KindSeeding, ADeclaredExtentRulesItsParameter) {
  const auto unit = collectUnit(R"c(
#define COUNTED_BY(n) __attribute__((annotate("weavec.counted_by." #n)))
int over(const int *COUNTED_BY(n) p, size_t n, size_t m) {
  int s = 0;
  for (size_t i = 0; i < m; i++)
    s += p[i];
  return s;
}
int upto(const int *COUNTED_BY(n) p, size_t n) {
  int s = 0;
  for (size_t i = 0; i < n; i++)
    s += p[i];
  return s;
}
)c");
  const Piped piped = pipe(unit);
  // §7.2: the declared `counted(n)` is what the calls check, so a loop to
  // `m` is the body's own check, never proven by its requirement.
  EXPECT_EQ(facetOf(piped, "over", "p[i]", core::Facet::Spatial),
            "checked:index");
  EXPECT_EQ(facetOf(piped, "upto", "p[i]", core::Facet::Spatial), "proven");
}

TEST(KindSeeding, AnExportedCalleesContractIsTrustedInside) {
  const auto unit = collectUnit(R"c(
int sum(const int *p, size_t n) {
  int s = 0;
  for (size_t i = 0; i < n; i++)
    s += p[i];
  return s;
}
int use(void) { int two[2] = {1, 2}; return sum(two, 3); }
)c");
  const Piped piped = pipe(unit);
  // §7.5: covered only by the inferred `counted(n)`, the access is the
  // callers' contract; its null facet stays checked.
  EXPECT_EQ(facetOf(piped, "sum", "p[i]", core::Facet::Spatial),
            "trusted/caller-contract");
  EXPECT_EQ(facetOf(piped, "sum", "p[i]", core::Facet::Null),
            "checked:nonnull");
  // A call the unit makes is checked against it.
  EXPECT_EQ(piped.diagnostics,
            (Lines{"8: 'sum' requires 12 bytes behind 'two', which has 8 "
                   "bytes"}));
}

TEST(KindSeeding, ACursorPassedWhereTheCalleeReliesOnSingleIsARow) {
  const auto unit = collectUnit(R"c(
int peek(const int *p) { return p[0]; }
int use(int k) { int a[4] = {1, 2, 3, 4}; return peek(a + k); }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(facetOf(piped, "use", "peek(a+k)", core::Facet::Spatial),
            "unresolved/unknown-extent");
}

// -- §7.4 rule 7: store groups --------------------------------------------

TEST(KindSeeding, AStoreGroupIsDecidedAfterItsLastStore) {
  const auto unit = collectUnit(R"c(
#define COUNTED_BY(n) __attribute__((annotate("weavec.counted_by." #n)))
struct buf { char *COUNTED_BY(cap) data; size_t cap; };
void grow(struct buf *b, size_t n) {
  char *p = malloc(n);
  if (!p) return;
  b->data = p;
  b->cap = n;
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(facetOf(piped, "grow", "b->data=p", core::Facet::Spatial),
            "proven");
}

} // namespace
} // namespace weavec::analysis
