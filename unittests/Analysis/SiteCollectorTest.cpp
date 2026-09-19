//===- SiteCollectorTest.cpp - Tests for SiteCollector --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0030 §2.1, §2.6: the sites of every emitted function, their kinds,
// ordinals, facets, exclusions and markings.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/SiteCollector.h"

#include "SiteTestUtils.h"

#include "clang/AST/Expr.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace weavec::analysis {

using test::collectUnit;
using test::describe;
using Lines = std::vector<std::string>;

static std::vector<std::string> emittedNames(const test::CollectedUnit &unit) {
  std::vector<std::string> names;
  for (const core::FunctionLedger &row : unit.sites.ledgers())
    names.push_back(row.name);
  return names;
}

namespace {

// §2.6 step 1: definitions CodeGen may emit, never system-header ones.
TEST(SiteCollector, SelectsEmittedFunctions) {
  const auto unit = collectUnit(R"c(
#include <sys.h>
void external(void) {}
static void unusedStatic(void) {}
static void usedStatic(void) {}
static inline void unusedInline(void) {}
static inline void usedInline(void) { sysInline(); }
__attribute__((used)) static void keptStatic(void) {}
inline int c99Inline(int x) { return x; }
extern inline __attribute__((gnu_inline)) int gnuInline(int x) { return x; }
void declared(void);
void caller(void) { usedStatic(); usedInline(); }
)c",
                                "static inline void sysInline(void) {}\n",
                                /*prelude=*/false);
  EXPECT_EQ(emittedNames(unit),
            (Lines{"external", "usedStatic", "usedInline", "keptStatic",
                   "c99Inline", "gnuInline", "caller"}));
  const core::FunctionLedger *row = unit.row("usedStatic");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->linkage, core::Linkage::Internal);
  EXPECT_EQ(unit.row("external")->linkage, core::Linkage::External);
  EXPECT_EQ(unit.row("external")->line, 3U);
}

// §2.1 Deref: `*p`, `p->f`, `p[0]`; only null when the lvalue's address is
// taken or it decays; `&*p` and the `offsetof` idiom make no site.
TEST(SiteCollector, Dereferences) {
  const auto unit = collectUnit(R"c(
struct s { int f; int arr[4]; int tail; };
unsigned long f(int *p, struct s *q, int (*rows)[4]) {
  int a = *p + q->f + p[0];
  int *b = &q->f;
  int *c = q->arr;
  int *d = &*p;
  unsigned long off = (unsigned long)&((struct s *)0)->tail;
  int e = rows[0][1];
  return a + *b + *c + *d + (unsigned long)off + e;
}
)c");
  EXPECT_EQ(
      describe(unit, "f"),
      (Lines{"deref *p spatial,null,temporal",
             "deref q->f spatial,null,temporal",
             "deref p[0] spatial,null,temporal", "deref q->f null",
             "deref q->arr null", "deref rows[0] null",
             "index rows[0][1] spatial,temporal",
             std::string("call/exit return") +
                 "a+*b+*c+*d+(unsignedlong)off+e temporal",
             "deref *b spatial,null,temporal", "deref *c spatial,null,temporal",
             "deref *d spatial,null,temporal"}));
}

// §2.1 Index: subscripts, `*(p + i)`; null and temporal only for pointer
// bases, temporal for arrays whose storage can end; `&p[i]` makes no site.
TEST(SiteCollector, Subscripts) {
  const auto unit = collectUnit(R"c(
struct s { int n; int arr[4]; int more; };
int g[8];
int f(int *p, struct s *q, int i) {
  int local[4] = {0};
  static int kept[2];
  int *at = &p[i];
  return p[i] + *(p + i) + local[i] + g[i] + q->arr[i] + kept[i] + *at +
         "abc"[i];
}
)c");
  EXPECT_EQ(
      describe(unit, "f"),
      (Lines{std::string("call/exit return") +
                 "p[i]+*(p+i)+local[i]+g[i]+q->arr[i]+" +
                 "kept[i]+*at+\"abc\"[i] temporal",
             "index p[i] spatial,null,temporal",
             "index *(p+i) spatial,null,temporal", "index local[i] spatial",
             "index g[i] spatial", "deref q->arr null",
             "index q->arr[i] spatial,temporal", "index kept[i] spatial",
             "deref *at spatial,null,temporal", "index \"abc\"[i] spatial"}));
}

// §2.1: the ordinal is the position in source order; operations that begin
// at the same place come inner first.
TEST(SiteCollector, OrdinalsFollowSourceOrder) {
  const auto unit = collectUnit(R"c(
struct n { struct n *next; int v; };
int f(struct n *p, int *q) {
  struct n x = { .v = *q, .next = p->next };
  return p->next->next->v + x.v;
}
)c");
  const core::FunctionLedger *row = unit.row("f");
  ASSERT_NE(row, nullptr);
  for (std::size_t i = 0; i < row->sites.size(); ++i)
    EXPECT_EQ(row->sites[i].ordinal, i);
  // The designated initialiser is walked in its semantic form (`.next`
  // first), yet `*q` comes first because it comes first in the source.
  EXPECT_EQ(describe(unit, "f"),
            (Lines{"deref *q spatial,null,temporal",
                   "deref p->next spatial,null,temporal",
                   "call/exit returnp->next->next->v+x.v temporal",
                   "deref p->next spatial,null,temporal",
                   "deref p->next->next spatial,null,temporal",
                   "deref p->next->next->v spatial,null,temporal"}));
}

// §2.1 LibCall and Release: facets as the row's requirements name them.
TEST(SiteCollector, LibraryCalls) {
  const auto unit = collectUnit(R"c(
void f(char *d, const char *s, FILE *file, char *local) {
  char buf[8];
  memcpy(d, s, 4);
  memcpy(buf, "abc", 4);
  (void)strlen(s);
  free(d);
  fclose(file);
  free(buf);
  char *m = malloc(4);
  (void)m;
  (void)local;
}
)c");
  EXPECT_EQ(describe(unit, "f"),
            (Lines{"lib-call memcpy(d,s,4) spatial,null,temporal",
                   "lib-call memcpy(buf,\"abc\",4) spatial",
                   "lib-call strlen(s) spatial,null,temporal",
                   "release free(d) spatial,temporal",
                   "release fclose(file) spatial,null,temporal",
                   "release free(buf) spatial", "lib-call malloc(4) -",
                   "call/exit } temporal"}));
  const core::FunctionLedger *row = unit.row("f");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->sites[0].callee, "memcpy");
  const SiteInfo *copy =
      unit.sites.info(core::SiteId{.function = 0, .ordinal = 0});
  ASSERT_NE(copy, nullptr);
  ASSERT_TRUE(copy->library.has_value());
  EXPECT_EQ(copy->library->entry->name, "memcpy");
  // memcpy's pointers may be null when the length is zero (§8.3).
  ASSERT_EQ(copy->arguments.size(), 2U);
  EXPECT_TRUE(copy->arguments[0].allowedIfZero);
  EXPECT_TRUE(copy->arguments[0].unlessZero.has_value());
}

// §2.1 Call: every other call; every exit, including calls that do not
// return; the end of the body only when control can reach it.
TEST(SiteCollector, CallsAndExits) {
  const auto unit = collectUnit(R"c(
_Noreturn void die(const char *why);
void user(int *p);
int helper(int *p) { return *p; }
int f(int *p, void (*cb)(int *), int c) {
  if (c)
    exit(1);
  if (c > 1)
    die("x");
  cb(p);
  user(p);
  return helper(p);
}
void g(int c) { if (c) return; }
void h(int c) { if (c) return; else return; }
void loop(void) { for (;;) {} }
)c");
  EXPECT_EQ(
      describe(unit, "f"),
      (Lines{"lib-call exit(1) -", "call/exit exit(1) temporal",
             "call/call die(\"x\") temporal", "call/exit die(\"x\") temporal",
             "call/call cb(p) null,temporal", "call/call user(p) temporal",
             "call/exit returnhelper(p) temporal",
             "call/call helper(p) temporal"}));
  EXPECT_EQ(describe(unit, "g"),
            (Lines{"call/exit return temporal", "call/exit } temporal"}));
  EXPECT_EQ(describe(unit, "h"),
            (Lines{"call/exit return temporal", "call/exit return temporal"}));
  EXPECT_EQ(describe(unit, "loop"), Lines{});
  const core::FunctionLedger *row = unit.row("f");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->sites[4].callee, "");
  EXPECT_EQ(row->sites[5].callee, "user");
}

// §2.1 Assume, IntToPtr and Raw.
TEST(SiteCollector, AssumeIntToPtrAndRaw) {
  const auto unit = collectUnit(R"c(
int *RAW mapped(void);
int f(unsigned long addr, int *RAW r, int n, ...) {
  ASSUME(n > 0);
  int *null = (int *)0;
  __builtin_va_list ap;
  __builtin_va_start(ap, n);
  int *v = __builtin_va_arg(ap, int *);
  __builtin_va_end(ap);
  free(r);
  return *(int *)addr + r[1] + *mapped() + *v + (null == 0);
}
)c");
  const Lines sites = describe(unit, "f");
  EXPECT_EQ(sites.front(), "assume ASSUME(n>0) assertion");
  EXPECT_NE(std::ranges::find(
                sites, "int-to-ptr __builtin_va_arg(ap,int*) spatial,temporal"),
            sites.end());
  EXPECT_NE(std::ranges::find(sites, "raw free(r) spatial,temporal"),
            sites.end());
  EXPECT_NE(std::ranges::find(sites, "raw *(int*)addr spatial,null,temporal"),
            sites.end());
  EXPECT_NE(std::ranges::find(sites, "int-to-ptr (int*)addr spatial,temporal"),
            sites.end());
  EXPECT_NE(std::ranges::find(sites, "raw r[1] spatial,null,temporal"),
            sites.end());
  EXPECT_NE(std::ranges::find(sites, "raw *mapped() spatial,null,temporal"),
            sites.end());
  // `(int *)0` is a null pointer constant, not a conversion from an integer.
  EXPECT_EQ(std::ranges::count_if(sites,
                                  [](const std::string &line) {
                                    return line.starts_with("int-to-ptr");
                                  }),
            2);
}

// §2.1 PtrArith and Cast: only in required positions (§7.4).
TEST(SiteCollector, RequiredPositions) {
  const auto unit = collectUnit(R"c(
struct buf { char *SIZED_BY(cap) data; unsigned long cap; char *plain; };
void take(int *SIZED_BY(n) p, int n);
void loose(int *p);
struct big { long a, b; };
void big(struct big *p __attribute__((nonnull)), int n);
void f(int *p, char *c, struct buf *b, int n) {
  take(p + 1, n);
  loose(p + 1);
  take(&p[2], n);
  b->data = c + 1;
  b->data = c;
  b->data = 0;
  b->plain = c + 1;
  ++b->data;
  struct buf local = { .data = c, .cap = 4 };
  (void)local;
}
)c");
  EXPECT_EQ(
      describe(unit, "f"),
      (Lines{"call/call take(p+1,n) spatial,temporal", "ptr-arith p+1 spatial",
             "call/call loose(p+1) temporal",
             "call/call take(&p[2],n) spatial,temporal",
             "ptr-arith &p[2] spatial", "deref b->data spatial,null,temporal",
             "ptr-arith c+1 spatial", "deref b->data spatial,null,temporal",
             "cast b->data=c spatial", "deref b->data spatial,null,temporal",
             "deref b->plain spatial,null,temporal",
             "ptr-arith ++b->data spatial",
             "deref b->data spatial,null,temporal", "cast c spatial",
             "call/exit } temporal"}));
}

// §2.1 exclusions: unevaluated operands, except variable-length arrays.
TEST(SiteCollector, UnevaluatedOperands) {
  const auto unit = collectUnit(R"c(
unsigned long f(int *p, int n, int (*vla)[n]) {
  unsigned long a = sizeof(*p) + _Alignof(int);
  unsigned long b = sizeof(*vla);
  int c = _Generic(0, int: 1, default: *p);
  int d = __builtin_choose_expr(1, 2, *p);
  int e = __builtin_constant_p(*p);
  return a + b + (unsigned long)(c + d + e);
}
)c");
  EXPECT_EQ(describe(unit, "f"),
            (Lines{"deref *vla null",
                   "call/exit returna+b+(unsignedlong)(c+d+e) temporal"}));
}

// §2.1: the shared operand of `a ?: b`, constant expressions and pointers
// into a non-default address space are marked.
TEST(SiteCollector, MarksSitesNoCheckCanServe) {
  const auto unit = collectUnit(R"c(
int arr[4];
int f(int **pp, __attribute__((address_space(1))) int *far, int i) {
  static int *fixed = &arr[1];
  int *p = *pp ?: arr;
  switch (i) { case sizeof(arr): return 0; }
  return *p + *far + *fixed;
}
)c");
  const SiteIndex::FunctionSites *function =
      unit.sites.function(*unit.function("f"));
  ASSERT_NE(function, nullptr);
  bool shared = false;
  bool far = false;
  for (const SiteInfo &site : function->sites) {
    if (site.sharedOperand) {
      shared = true;
      EXPECT_EQ(site.kind, core::SiteKind::Deref);
    }
    far = far || site.nonDefaultAddressSpace;
  }
  EXPECT_TRUE(shared);
  EXPECT_TRUE(far);
}

// §6.1, §5.4: unsafe regions and returns-twice callers are marked.
TEST(SiteCollector, MarksUnsafeRegionsAndSetjmp) {
  const auto unit = collectUnit(R"c(
jmp_buf env;
int f(int *p) {
  int x = *p;
  UNSAFE { x += p[1]; }
  return x;
}
UNSAFE int g(int *p) { return *p; }
int h(int *p) { if (setjmp(env)) return 0; return *p; }
)c");
  const auto flags = [&](const char *name) {
    std::vector<bool> out;
    for (const SiteInfo &site :
         unit.sites.function(*unit.function(name))->sites)
      out.push_back(site.inUnsafe);
    return out;
  };
  EXPECT_EQ(flags("f"), (std::vector<bool>{false, true, false}));
  EXPECT_EQ(flags("g"), (std::vector<bool>{true, true}));
  EXPECT_FALSE(unit.sites.function(*unit.function("f"))->callsSetjmp);
  EXPECT_TRUE(unit.sites.function(*unit.function("h"))->callsSetjmp);
  EXPECT_TRUE(unit.row("h")->callsSetjmp);
}

// RFC 0030 §6.3: `WEAVEC_REQUIRE_SAFE` marks the function's row, which
// `LedgerAdapter` holds to `checked`.
TEST(SiteCollector, MarksRequireSafeFunctions) {
  const auto unit = collectUnit(R"c(
#define REQUIRE_SAFE __attribute__((annotate("weavec.require_safe")))
REQUIRE_SAFE int strict(int *p) { return *p; }
int lax(int *p) { return *p; }
)c");
  ASSERT_NE(unit.row("strict"), nullptr);
  ASSERT_NE(unit.row("lax"), nullptr);
  EXPECT_TRUE(unit.row("strict")->requireSafe);
  EXPECT_FALSE(unit.row("lax")->requireSafe);
}

// The index finds every site by statement, and the exit of a call that does
// not return separately.
TEST(SiteCollector, IndexesSitesByStatement) {
  const auto unit = collectUnit(R"c(
int f(int *p) {
  if (!p)
    exit(2);
  return *p;
}
)c");
  const SiteIndex::FunctionSites *function =
      unit.sites.function(*unit.function("f"));
  ASSERT_NE(function, nullptr);
  ASSERT_EQ(function->sites.size(), 4U);
  const auto *call = llvm::cast<clang::CallExpr>(function->sites[0].stmt);
  EXPECT_EQ(unit.sites.sitesOf(*call).size(), 2U);
  EXPECT_EQ(unit.sites.find(*call)->ordinal, 0U);
  EXPECT_EQ(unit.sites.findExit(*call)->ordinal, 1U);
  EXPECT_EQ(unit.sites.find(*call, core::SiteKind::LibCall)->ordinal, 0U);
  const clang::Stmt *ret = function->sites[2].stmt;
  EXPECT_TRUE(llvm::isa<clang::ReturnStmt>(ret));
  EXPECT_EQ(unit.sites.find(*ret)->ordinal, 2U);
  EXPECT_EQ(
      unit.sites.info(core::SiteId{.function = function->index, .ordinal = 3})
          ->kind,
      core::SiteKind::Deref);
  // `weavec_assume_` is unused here, so `f` is the only emitted function.
  EXPECT_EQ(unit.sites.functions().size(), 1U);
  EXPECT_EQ(unit.sites.siteCount(), 4U);
}

// §2.6: a spatial facet defaults to checked only against an extent exact
// from the type or declared over unmodified parameters and constants.
TEST(SiteCollector, SpatialDefaults) {
  const auto unit = collectUnit(R"c(
struct flex { int n; int tail[1]; };
struct fixed { int a[4]; int n; };
int f(int *SIZED_BY(n) p, int n, int *q, int *SIZED_BY(m) r, int m,
      struct flex *x, struct fixed *y, int i) {
  int local[3] = {0};
  r = q;
  return local[i] + p[i] + q[i] + r[i] + x->tail[i] + y->a[i] + local[1];
}
)c");
  const SiteIndex::FunctionSites *function =
      unit.sites.function(*unit.function("f"));
  ASSERT_NE(function, nullptr);
  std::vector<std::string> defaults;
  for (const SiteInfo &site : function->sites) {
    if (site.kind != core::SiteKind::Index)
      continue;
    std::string text = unit.row("f")->sites[site.id.ordinal].text + ":";
    if (!site.spatialCheckable())
      text += "none";
    else
      text += std::string(
                  core::toString(site.spatialDefaults.front().extentClass)) +
              " " + site.spatialDefaults.front().extent->toString();
    if (site.provenByType)
      text += " by-type";
    defaults.push_back(text);
  }
  EXPECT_EQ(defaults, (Lines{"local[i]:exact 3", "p[i]:declared n", "q[i]:none",
                             "r[i]:none", "x->tail[i]:none", "y->a[i]:exact 4",
                             "local[1]:exact 3 by-type"}));
}

// §2.6 for calls: a spatial facet defaults to checked when every
// requirement compares simple terms against an extent the types or a
// declaration give.
TEST(SiteCollector, CallSpatialDefaults) {
  const auto unit = collectUnit(R"c(
void take(int *SIZED_BY(n) p, int n);
void f(char *d, const char *s, unsigned long n, int *q) {
  char buf[8];
  char other[8];
  int ints[4];
  memcpy(buf, other, n);
  memcpy(d, s, n);
  take(ints, 4);
  take(q, 4);
  (void)strlen(buf);
}
)c");
  const SiteIndex::FunctionSites *function =
      unit.sites.function(*unit.function("f"));
  ASSERT_NE(function, nullptr);
  Lines defaults;
  for (const SiteInfo &site : function->sites) {
    if (site.kind != core::SiteKind::LibCall &&
        site.kind != core::SiteKind::Call)
      continue;
    std::string line = unit.row("f")->sites[site.id.ordinal].text + ":";
    for (const CheckWitness &witness : site.spatialDefaults) {
      line += witness.shape == CheckWitness::Shape::Length ? " length#"
                                                           : " disjoint#";
      line += std::to_string(*witness.argument);
      if (witness.extent)
        line += " " + witness.extent->toString();
    }
    defaults.push_back(line);
  }
  EXPECT_EQ(
      defaults,
      (Lines{std::string(
                 "memcpy(buf,other,n): length#0 sizeof(char[8]) length#1 ") +
                 "sizeof(char[8]) disjoint#0",
             "memcpy(d,s,n):", "take(ints,4): length#0 sizeof(int[4])",
             "take(q,4):", "strlen(buf):", "}:"}));
}

} // namespace
} // namespace weavec::analysis
