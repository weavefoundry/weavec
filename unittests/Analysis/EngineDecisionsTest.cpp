//===- EngineDecisionsTest.cpp - The engine's decisions (RFC 0030) --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §15 items 3, 4, 12 and 15 (stage S3-B2): what the engine decides
// about every site it reaches, the requirement records of calls, the
// witnesses its checks get (seen as the planned template), raw-cast
// pointers, and the §7.4 object extents.
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

/// A decision as `<outcome>[/<reason>][:<template>]`.
static std::string spell(const core::FacetDecision &decision,
                         const std::optional<core::FacetCheck> &check) {
  std::string text = decision.compact();
  if (check)
    text += ":" + std::string(core::toString(check->kind));
  return text;
}

/// `<text> <facet>=<decision>` and the requirement records (`{a0=...}`) for
/// every site of `function`.
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
                spell(record->decision, record->check);
        if (record->requirements.empty())
          continue;
        line += '{';
        bool first = true;
        for (const core::Requirement &requirement : record->requirements) {
          if (!first)
            line += ',';
          first = false;
          line += "a" +
                  (requirement.argument ? std::to_string(*requirement.argument)
                                        : std::string("?")) +
                  "=" + spell(requirement.decision, requirement.check);
        }
        line += '}';
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

static Piped pipe(const test::CollectedUnit &unit) {
  core::DiagnosticCollector collected;
  const UnitPipelineResult result =
      runUnitAnalysis(unit.context(), UnitPipelineOptions{}, collected);
  Piped out;
  for (const core::Diagnostic &d : collected.diagnostics())
    out.diagnostics.push_back(std::to_string(d.location.line) + ": " +
                              std::string(core::toString(d.severity)) + ": " +
                              d.message);
  if (result.ledger)
    out.ledger = result.ledger->ledger;
  return out;
}

/// The line of `outcomes(ledger, function)` whose text is `text`, or "".
static std::string row(const Piped &piped, llvm::StringRef function,
                       llvm::StringRef text) {
  for (const std::string &line : outcomes(piped.ledger, function))
    if (llvm::StringRef(line).starts_with((text + " ").str()))
      return line;
  return {};
}

namespace {

// -- §15 item 4: every site the pass reaches -------------------------------

TEST(EngineDecisions, InteriorAndConsumedAccessesAreDecided) {
  const auto unit = collectUnit(R"c(
struct vec { int *items; int n; };
int at(struct vec *v, int i) { return v->items[i]; }
void drop(char **a, int i) { free(a[i]); }
int local(void) { struct vec v = {0, 0}; struct vec *p = &v; return p->n; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty());
  // The load of `v->items` on the way to the element has its own spatial
  // decision (a parameter's extent is S6's), and so has the consumed load.
  EXPECT_EQ(row(piped, "at", "v->items"),
            "v->items spatial=unresolved/unknown-extent null=checked:nonnull "
            "temporal=proven");
  EXPECT_EQ(row(piped, "drop", "a[i]"),
            "a[i] spatial=unresolved/unknown-extent null=checked:nonnull "
            "temporal=proven");
  EXPECT_EQ(row(piped, "local", "p->n"),
            "p->n spatial=proven null=proven temporal=proven");
}

TEST(EngineDecisions, AUseAfterFreeStillDecidesItsNullFacet) {
  const auto unit = collectUnit(R"c(
int f(char *p) { free(p); return p[1]; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(row(piped, "f", "p[1]"),
            "p[1] spatial=unresolved/unknown-extent null=checked:nonnull "
            "temporal=violation");
}

// §15 item 4: the start of an allocation, proven when the engine knows it.
TEST(EngineDecisions, ReleasesAreTheStartOfTheirAllocation) {
  const auto unit = collectUnit(R"c(
void own(void) { char *p = malloc(4); free(p); }
void none(void) { free(0); }
void param(char *p) { free(p); }
void inner(void) { char *p = malloc(4); if (!p) return; free(p + 1); }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(row(piped, "own", "free(p)"),
            "free(p) spatial=proven temporal=proven");
  EXPECT_EQ(row(piped, "none", "free(0)"),
            "free(0) spatial=proven temporal=proven");
  EXPECT_EQ(row(piped, "param", "free(p)"),
            "free(p) spatial=unresolved/unknown-index temporal=proven");
  EXPECT_EQ(row(piped, "inner", "free(p+1)"),
            "free(p+1) spatial=violation temporal=proven");
}

// -- §2.5, §8, §15 item 12: requirement records of calls ---------------------

TEST(EngineRequirements, LibraryCallsHaveARecordPerRequirement) {
  const auto unit = collectUnit(R"c(
char *strcpy(char *, const char *);
void copy(const char *src, size_t n) { char buf[16]; memcpy(buf, src, n); }
void fits(void) { char buf[16]; strcpy(buf, "hello"); }
void none(char *d, const char *s) { memcpy(d, s, 0); }
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty())
      << ::testing::PrintToString(piped.diagnostics);
  // The destination is checked against its 16 bytes, the source's extent is
  // unknown, and the two may overlap: a checked record is planned even when
  // another requirement of the call is unresolved (§2.5).
  EXPECT_EQ(row(piped, "copy", "memcpy(buf,src,n)"),
            "memcpy(buf,src,n) spatial=unresolved/unknown-extent{"
            "a0=checked:len,a1=unresolved/unknown-extent,a0=checked:disjoint} "
            "null=checked:nonnull{a1=checked:nonnull} temporal=proven");
  // Arrays in scope have no null or temporal facet (§2.1).
  EXPECT_EQ(row(piped, "fits", "strcpy(buf,\"hello\")"),
            "strcpy(buf,\"hello\") spatial=proven{a0=proven,a1=proven,"
            "a0=proven}");
  EXPECT_EQ(row(piped, "none", "memcpy(d,s,0)"),
            "memcpy(d,s,0) spatial=proven{a0=proven,a1=proven,a0=proven} "
            "null=checked:nonnull{a0=checked:nonnull,a1=checked:nonnull} "
            "temporal=proven");
}

// A declared kind (`SIZED_BY`) is a requirement of the call (§7.2).
TEST(EngineRequirements, DeclaredRequirementsAreCheckedAtTheCall) {
  const auto unit = collectUnit(R"c(
void fill(char *SIZED_BY(n) dst, size_t n) { dst[0] = 0; }
void call(size_t n) { char buf[8]; fill(buf, n); fill(buf, 4); }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(row(piped, "call", "fill(buf,n)"),
            "fill(buf,n) spatial=checked:len{a0=checked:len} temporal=proven");
  EXPECT_EQ(row(piped, "call", "fill(buf,4)"),
            "fill(buf,4) spatial=proven{a0=proven} temporal=proven");
}

// RFC 0030 *Diagnostics*: a string literal has no writable byte.
TEST(EngineRequirements, WritesIntoAStringLiteralAreViolations) {
  const auto unit = collectUnit(R"c(
char *strcpy(char *, const char *);
void store(void) { char *p = "abc"; p[0] = 'x'; }
void copy(void) { char *p = "abc"; strcpy(p, "z"); }
void maybe(int c, char *b) { char *p = c ? "abc" : b; p[0] = 'x'; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"3: error: write through 'p', which points to a string "
                   "literal",
                   "4: error: write through 'p', which points to a string "
                   "literal"}));
  EXPECT_EQ(row(piped, "store", "p[0]"),
            "p[0] spatial=violation null=proven temporal=proven");
  EXPECT_EQ(row(piped, "maybe", "p[0]"),
            "p[0] spatial=unresolved/unknown-extent null=checked:nonnull "
            "temporal=proven");
}

// -- §2.3 `raw-cast`: pointers made by reinterpretation ---------------------

TEST(EngineRawCast, ReinterpretedPointersAreUnresolved) {
  const auto unit = collectUnit(R"c(
union pun { long bits; int *p; };
int punned(union pun *u) { u->bits = 1234; return *u->p; }
int bytes(void) {
  char *p = malloc(8); if (!p) return 0;
  char *q; unsigned char *d = (unsigned char *)&q, *s = (unsigned char *)&p;
  for (int i = 0; i < 8; i++) d[i] = s[i];
  return q[0];
}
int vararg(int n, ...) {
  __builtin_va_list ap; __builtin_va_start(ap, n);
  int *p = __builtin_va_arg(ap, int *);
  int *copy = p;
  __builtin_va_end(ap);
  return *copy;
}
int plain(union pun *u) { u->p = 0; return 0; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(row(piped, "punned", "*u->p"),
            "*u->p spatial=unresolved/raw-cast null=checked:nonnull "
            "temporal=unresolved/raw-cast");
  EXPECT_EQ(row(piped, "bytes", "q[0]"),
            "q[0] spatial=unresolved/raw-cast null=checked:nonnull "
            "temporal=unresolved/raw-cast");
  EXPECT_EQ(row(piped, "vararg", "*copy"),
            "*copy spatial=unresolved/raw-cast null=checked:nonnull "
            "temporal=unresolved/raw-cast");
}

// -- §7.4 and §14: object extents and the witnesses of checks ---------------

// §7.4 *Arithmetic*: the allocation holds the bytes the program passed; a
// count the facts do not give is those bytes rounded down (probe 06).
TEST(EngineExtents, AWrappedByteCountIsCheckedAsBytes) {
  const auto unit = collectUnit(R"c(
void fill(unsigned n) {
  unsigned bytes = n * 4u;
  int *a = malloc(bytes);
  if (!a) return;
  for (unsigned i = 0; i < n; i++) a[i] = 0;
  free(a);
}
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(row(piped, "fill", "a[i]"),
            "a[i] spatial=checked:index null=proven temporal=proven");
}

// §7.4: a cursor into its object is checked with a span over the object;
// a pointer converted between element sizes keeps no element offset.
TEST(EngineExtents, CursorsAreCheckedAgainstTheirObject) {
  const auto unit = collectUnit(R"c(
int walk(int i) { int a[8] = {0}; int *p = a + 2; return p[i]; }
int flat(void) {
  int m[3][4] = {{0}}; int *q = &m[0][0]; int s = 0;
  for (int k = 0; k < 12; k++) s += q[k];
  return s;
}
char bytes(void) { int a[10] = {0}; char *c = (char *)(a + 2); return c[35]; }
)c");
  const Piped piped = pipe(unit);
  EXPECT_TRUE(piped.diagnostics.empty())
      << ::testing::PrintToString(piped.diagnostics);
  EXPECT_EQ(row(piped, "walk", "p[i]"),
            "p[i] spatial=checked:span null=proven temporal=proven");
  EXPECT_EQ(row(piped, "flat", "q[k]"),
            "q[k] spatial=checked:span null=proven temporal=proven");
  // Not proven from `a`'s element offset in chars; the span measures it.
  EXPECT_EQ(row(piped, "bytes", "c[35]"),
            "c[35] spatial=checked:span null=proven temporal=proven");
}

// §7.4: a trailing array member is flexible whatever its bound; a pointer
// made from a member has the extent of the whole object; only a direct
// subscript of a non-flexible member array uses the member's bound.
TEST(EngineExtents, MembersAreBoundedByTheirObject) {
  const auto unit = collectUnit(R"c(
struct fixed { int pad; int data[2]; };
struct mid { int items[4]; int n; };
struct s3 { int first; int second; char *name; };
void trailing(void) { struct fixed *b = malloc(100); if (!b) return; b->data[2] = 1; free(b); }
void direct(void) { struct mid *g = malloc(sizeof *g); if (!g) return; g->items[4] = 1; free(g); }
void decayed(void) { struct mid *g = malloc(sizeof *g); if (!g) return; int *p = g->items; p[4] = 1; free(g); }
void member(void) { struct s3 s; int *p = &s.second; p[1] = 2; }
void flexible(int n) { struct fixed *b = malloc(sizeof *b + 4 * (size_t)n); if (!b) return; b->data[5] = 1; free(b); }
)c");
  const Piped piped = pipe(unit);
  EXPECT_EQ(piped.diagnostics,
            (Lines{"6: error: 'g->items[4]' is out of bounds: index 4 of an "
                   "object of 16 bytes"}));
  EXPECT_EQ(row(piped, "trailing", "b->data[2]"),
            "b->data[2] spatial=proven temporal=proven");
  EXPECT_EQ(row(piped, "decayed", "p[4]"),
            "p[4] spatial=proven null=checked:nonnull temporal=proven");
  EXPECT_EQ(row(piped, "member", "p[1]"),
            "p[1] spatial=proven null=proven temporal=proven");
  // The flexible member's elements up to the end of the allocation.
  EXPECT_EQ(row(piped, "flexible", "b->data[5]"),
            "b->data[5] spatial=checked:index temporal=proven");
}

} // namespace
} // namespace weavec::analysis
