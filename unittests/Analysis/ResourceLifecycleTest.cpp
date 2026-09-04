//===- ResourceLifecycleTest.cpp - Leaks, families, owned fields ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0007: the checker's resource lifecycle rules. Each test names the RFC
// section it pins.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Core/Diagnostic.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace weavec::test {
namespace {

using Strings = std::vector<std::string>;

/// The libc surface the tests need beyond `Prelude`; matched by name against
/// the shipped table (RFC 0003).
constexpr const char *Libc = R"c(
typedef struct FILE FILE;
FILE *fopen(const char *, const char *);
int fclose(FILE *);
char *strdup(const char *);
int getline(char **, size_t *, FILE *);
void exit(int) __attribute__((noreturn));
void register_thing(void *);
#line 1
)c";

// -- Leaks (RFC 0007, *Bugs caught*) -----------------------------------------

TEST(ResourceLifecycle, LeaksAreReportedWhereTheResourceIsLost) {
  const auto result = analyze(std::string(Libc) + R"c(
    int leak_path(int c) {
      char *p = malloc(8);
      if (c)
        return -1;
      free(p);
      return 0;
    }
    void overwrite(void) {
      char *p = malloc(8);
      p = malloc(16);
      free(p);
    }
    void owned_param(char *OWNED p) { use(p); }
    void discarded(const char *s) { strdup(s); }
    void scope_end(void) {
      char *p = malloc(8);
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"leak", "leak", "leak", "leak", "leak"}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: 'p' is leaked",
                     "11: 'p' is leaked: it is overwritten without being "
                     "released",
                     "14: 'p' is leaked", "15: result of 'strdup' is leaked",
                     "18: 'p' is leaked"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"allocated here"}));
  EXPECT_EQ(notes(result.diagnostics, 2),
            (Strings{"'p' is declared WEAVEC_OWNED here"}));
  EXPECT_TRUE(notes(result.diagnostics, 3).empty())
      << "a discarded result has no holder to point at";
}

TEST(ResourceLifecycle, OneReportPerResource) {
  const auto result = analyze(R"c(
    void copies(void) {
      char *p = malloc(8);
      char *q = p;
      use(p);
      use(q);
    }
    void both_arms(int c) {
      char *p = malloc(8);
      use(p);
      if (c)
        return;
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // The record travels with copies: one report, for the holder that dies
  // last. A resource lost on two paths is reported once per path (RFC 0007,
  // *Accepted false positives*).
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"6: 'q' is leaked", "12: 'p' is leaked", "13: 'p' is leaked"}));
}

TEST(ResourceLifecycle, NeverReadValuesAreLostAtOnce) {
  const auto result = analyze(R"c(
    void f(int c) {
      char *p = malloc(8);
      if (c)
        use(0);
    }
    void g(void) {
      char *p;
      p = malloc(8);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'p' is leaked", "9: 'p' is leaked"}));
}

// -- What is not a leak (RFC 0007, *Deliberately not caught*) -----------------

TEST(ResourceLifecycle, ReleasedMovedReturnedOrStoredIsNotLost) {
  const auto result = analyze(std::string(Libc) + R"c(
    struct buf { char *data; };
    int checked(void) {
      char *p = malloc(8);
      if (!p)
        return -1;
      free(p);
      return 0;
    }
    char *handed_out(void) { char *p = malloc(8); return p; }
    void stored(struct buf *b) { b->data = malloc(8); }
    void out_param(char **out) { *out = malloc(8); }
    void moved(void) { char *p = malloc(8); take(p); }
    void kept(void) { static char *keep; char *p = malloc(8); keep = p; }
    char *global;
    void in_global(void) { global = malloc(8); }
    void by_callee(void) { char *p = malloc(8); if (p) free(p); }
    struct buf make(void) { struct buf b; b.data = malloc(8); return b; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

TEST(ResourceLifecycle, EscapesAreNotLeaks) {
  const auto result = analyze(std::string(Libc) + R"c(
    typedef unsigned long uintptr_t;
    void to_unknown(void) { char *p = malloc(8); register_thing(p); }
    void to_int(void) { char *p = malloc(8); uintptr_t u = (uintptr_t)p; (void)u; }
    void to_raw(void *RAW r);
    void via_raw(void) { char *p = malloc(8); to_raw(p); }
    void exits(void) { char *p = malloc(8); if (!p) exit(1); use(p); exit(0); }
    void fatal(const char *m) __attribute__((noreturn));
    void dies_into_fatal(char *q) {
      char *p = malloc(8);
      if (q == NULL) fatal("no q");
      free(p);
    }
    struct box { char *buf; };
    struct box *lookup(int);
    void below_opaque(int i) {
      struct box *b = lookup(i);
      b->buf = malloc(8);
    }
    void below_opaque_copy(int i) {
      struct box *b = lookup(i);
      char *p = malloc(8);
      b->buf = p;
    }
  )c");
  ASSERT_TRUE(result.ast);
  // Only the boundary warnings for the unknown callees.
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"annotation-required", "annotation-required",
                     "annotation-required"}));
}

// `tb = &L->strt; tb->hash = fresh`: the store lands in the caller's object
// under a name the summary cannot report, so the value escapes; the same
// store into a borrowed local is still this function's to release.
TEST(ResourceLifecycle, StoresBelowABorrowOfCallerMemoryEscape) {
  const auto result = analyze(std::string(Libc) + R"c(
    struct tb { char **hash; int size; };
    struct G { struct tb strt; };
    struct G global;
    void resize(struct G *g, int nsize, int osize) {
      struct tb *tb = &g->strt;
      char **newvect = malloc(nsize);
      if (newvect == NULL) return;
      tb->hash = newvect;
      tb->size = nsize;
      if (nsize > osize) use(newvect);
    }
    void resize_global(int nsize) {
      struct tb *tb = &global.strt;
      tb->hash = malloc(nsize);
    }
    void resize_local(int nsize) {
      struct tb local;
      struct tb *tb = &local;
      char **newvect = malloc(nsize);
      if (newvect == NULL) return;
      tb->hash = newvect;
      use(newvect);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), (Strings{"23: 'newvect' is leaked"}));
}

TEST(ResourceLifecycle, StoresBelowKnownPointersAreTracked) {
  const auto result = analyze(std::string(Libc) + R"c(
    struct box { char *buf; };
    void below_fresh(void) {
      struct box *b = malloc(sizeof *b);
      if (!b) return;
      b->buf = malloc(8);
      free(b);
    }
    void below_param(struct box *b) { b->buf = malloc(8); }
    void below_copy(struct box *outer) {
      struct box *b = outer;
      b->buf = malloc(8);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // The fresh object's field is lost with it; the other two are stores into
  // caller memory (the parameter's, or an alias of it), not leaks.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: 'b->buf' is leaked when 'b' is freed"}));
  const core::FunctionSummary *param = result.summary("below_param");
  ASSERT_NE(param, nullptr);
  EXPECT_TRUE(std::ranges::any_of(param->stores, [](const core::Store &store) {
    return store.dest == core::SummaryPath::param(0).deref().field("buf") &&
           store.value.kind == core::ValueSource::Kind::Fresh;
  }));
}

TEST(ResourceLifecycle, NullTestsSeeThroughBuiltinExpectAndTruthComparisons) {
  const auto result = analyze(std::string(Libc) + R"c(
    int expect_null(void) {
      char *p = malloc(8);
      if (__builtin_expect((p == NULL), 0)) return -1;
      free(p);
      return 0;
    }
    int compared_with_zero(void) {
      char *p = malloc(8);
      if ((p != NULL) == 0) return -1;
      free(p);
      return 0;
    }
    int nested(void) {
      char *p = malloc(8);
      if (__builtin_expect((p == NULL) != 0, 0)) return -1;
      free(p);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

TEST(ResourceLifecycle, ALogicalOperatorComputedAsAValueDecidesItsOperands) {
  // Under `__builtin_expect`, `!` or `!= 0` the `&&` is a value the branch
  // tests, not the operand the short-circuit CFG evaluated last: a true `&&`
  // makes both operands true (Lua's `luaM_realloc_`, where the fallback
  // overwrites a null `newblock`), a false one says nothing about either.
  const auto result = analyze(std::string(Libc) + R"c(
    void *grow(void *block, size_t n) {
      void *fresh = realloc(block, n);
      if (__builtin_expect((fresh == NULL && n > 0), 0))
        fresh = malloc(n);
      return fresh;
    }
    int and_false_says_nothing(size_t n) {
      char *p = malloc(8);
      if (__builtin_expect((n > 0 && p != NULL), 1)) { free(p); return 0; }
      return -1;
    }
    int or_true_says_nothing(size_t n) {
      char *p = malloc(8);
      if (!(p == NULL || n == 0)) { free(p); return 0; }
      return -1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"11: 'p' is leaked", "16: 'p' is leaked"}));
}

TEST(ResourceLifecycle, ABodyThatKeepsNothingIsTrusted) {
  const auto result = analyze(R"c(
    static int peeks(char *p) { return p != NULL; }
    static void keep_it(char **slot, char *p) { *slot = p; }
    int trusted(void) {
      char *p = malloc(8);
      int r = peeks(p);
      free(p);
      return r;
    }
    void stored_by_callee(char **slot) {
      char *p = malloc(8);
      keep_it(slot, p);
    }
    int lost(void) {
      char *p = malloc(8);
      return peeks(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // `peeks` records no effect on `p`: passing it is not an escape, so `lost`
  // is a leak; `keep_it` stores it where the caller can see (RFC 0007,
  // *Assumptions*).
  EXPECT_EQ(messages(result.diagnostics), (Strings{"16: 'p' is leaked"}));
}

TEST(ResourceLifecycle, NullTestsClearTheRecordOnTheNullEdge) {
  const auto result = analyze(std::string(Libc) + R"c(
    int bang(void) { char *p = malloc(8); if (!p) return -1; free(p); return 0; }
    int eq(void) { char *p = malloc(8); if (p == NULL) return -1; free(p); return 0; }
    int assign(void) { char *p; if (!(p = malloc(8))) return -1; free(p); return 0; }
    void lines(FILE *f) {
      char *l = NULL; size_t n = 0;
      while (getline(&l, &n, f) != -1) {}
      free(l);
    }
    void goto_cleanup(void) {
      char *a = NULL, *b = NULL;
      a = malloc(8); if (!a) goto out;
      b = malloc(8); if (!b) goto out;
    out:
      free(a); free(b);
    }
    int short_circuit(int fd) {
      char *path;
      if (fd == -1 || (path = (char *)malloc(16)) == NULL) return -1;
      free(path);
      return 0;
    }
    int nested(int fd, int mode) {
      char *path;
      if ((fd == -1 && mode) || !(path = malloc(16))) return -1;
      free(path);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

TEST(ResourceLifecycle, ReallocFailurePathIsNotALeak) {
  const auto result = analyze(R"c(
    void grow(char *OWNED p, size_t n) {
      char *q = realloc(p, n);
      if (!q) { free(p); return; }
      free(q);
    }
    int in_place(char **pp, size_t n) {
      char *q = realloc(*pp, n);
      if (!q) return -1;
      *pp = q;
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

// -- Mismatched releases (RFC 0007, *Release families*) -----------------------

TEST(ResourceLifecycle, MismatchedReleaseIsAnError) {
  const auto result = analyze(std::string(Libc) + R"c(
    static void xfree(void *p) { free(p); }
    void file_freed(const char *path) {
      FILE *f = fopen(path, "r");
      if (!f) return;
      free(f);
    }
    void memory_closed(void) {
      char *p = malloc(8);
      if (!p) return;
      fclose((FILE *)p);
    }
    void reallocated(const char *path) {
      FILE *f = fopen(path, "r");
      if (!f) return;
      f = realloc(f, 16);
      free(f);
    }
    void through_wrapper(const char *path) {
      FILE *f = fopen(path, "r");
      if (!f) return;
      xfree(f);
    }
    void fine(const char *path) {
      FILE *f = fopen(path, "r");
      if (f) fclose(f);
      char *s = strdup(path);
      xfree(s);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"mismatched-release", "mismatched-release",
                     "mismatched-release", "mismatched-release"}));
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "6: 'f' is released with 'free' but must be released with 'fclose'",
          "11: 'p' is released with 'fclose' but must be released with 'free'",
          "16: 'f' is released with 'free' but must be released with 'fclose'",
          "22: 'f' is released with 'free' but must be released with "
          "'fclose'"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"allocated here"}));
  for (const core::Diagnostic &d : result.diagnostics.diagnostics())
    EXPECT_EQ(d.severity, core::Severity::Error);

  const core::FunctionSummary *xfree = result.summary("xfree");
  ASSERT_NE(xfree, nullptr);
  EXPECT_EQ(xfree->effectOf(core::SummaryPath::param(0)).family, "free");
}

TEST(ResourceLifecycle, FamiliesAreInferredForWrappers) {
  const auto result = analyze(std::string(Libc) + R"c(
    FILE *open_log(const char *path) { return fopen(path, "a"); }
    char *dup_or_fresh(const char *s, int c) { return c ? strdup(s) : malloc(4); }
    void *either(int c) { return c ? (void *)fopen("x", "r") : malloc(4); }
  )c");
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("open_log"), nullptr);
  EXPECT_EQ(result.summary("open_log")->freshReturnFamily(), "fclose");
  ASSERT_NE(result.summary("dup_or_fresh"), nullptr);
  EXPECT_EQ(result.summary("dup_or_fresh")->freshReturnFamily(), "free")
      << "strdup and malloc are one family";
  ASSERT_NE(result.summary("either"), nullptr);
  EXPECT_EQ(result.summary("either")->freshReturnFamily(), "")
      << "two families join to unknown, which is never reported";
}

// -- Owned fields (RFC 0007, *Owned fields*) ----------------------------------

TEST(ResourceLifecycle, OwnedFieldsAreLostWithTheirContainer) {
  const auto result = analyze(std::string(Libc) + R"c(
    struct box { char *OWNED p; };
    struct buf { char *data; };
    void declared(struct box *b) { free(b); }
    void inferred(struct buf *b) { b->data = malloc(8); free(b); }
    void elements(char **a) { a[0] = strdup("x"); free(a); }
    void released(struct box *b) { free(b->p); free(b); }
    void nulled(struct box *b) { free(b->p); b->p = NULL; free(b); }
    void maybe(struct box *b) { if (b->p) free(b->p); free(b); }
    void moved_out(struct box *b, char **out) { *out = b->p; free(b); }
    void fresh(void) { struct box *b = malloc(sizeof *b); if (b) free(b); }
    void init(struct box *b, char *OWNED p) { b->p = p; }
    void forgotten(struct buf *b) { b->data = malloc(8); b->data = NULL; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'b->p' is leaked when 'b' is freed",
                     "5: 'b->data' is leaked when 'b' is freed",
                     "6: '*a' is leaked when 'a' is freed",
                     "13: 'b->data' is leaked: it is overwritten without "
                     "being released"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"'p' is declared WEAVEC_OWNED here"}));
  EXPECT_EQ(notes(result.diagnostics, 1), (Strings{"allocated here"}));
}

TEST(ResourceLifecycle, DeclaredOwnedFieldsOfFreshObjectsOwnNothing) {
  const auto result = analyze(R"c(
    struct box { char *OWNED p; };
    static struct box *box_new(void) { return malloc(sizeof(struct box)); }
    struct box *ctor(void) {
      struct box *b = malloc(sizeof *b);
      if (!b) return NULL;
      b->p = malloc(4);
      if (!b->p) { free(b); return NULL; }
      return b;
    }
    void from_callee(void) { struct box *b = box_new(); if (b) free(b); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

// -- Deepest paths first (RFC 0007, *Applying a summary*) ---------------------

TEST(ResourceLifecycle, CalleeFreeingFieldAndObjectFreesTheCallersAlias) {
  const auto result = analyze(R"c(
    struct box { char *p; };
    static void both(struct box *b) { free(b->p); free(b); }
    static void both_reversed(struct box *b) { char *t = b->p; free(b); free(t); }
    static void only_field(struct box *b) { free(b->p); }
    void f(struct box *b) { char *q = b->p; both(b); q[0] = 1; }
    void g(struct box *b) { char *q = b->p; both_reversed(b); q[0] = 1; }
    void h(struct box *b) { char *q = b->p; only_field(b); q[0] = 1; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: use of 'q' after it was freed",
                     "7: use of 'q' after it was freed",
                     "8: use of 'q' after it was freed"}));
}

TEST(ResourceLifecycle, TwoSummaryPathsNamingOneCellAreOneRelease) {
  // `g->twups ~ L` in the caller makes `L->g->allgc` and
  // `L->g->twups->g->allgc` one cell; the callee's summary names both.
  const auto result = analyze(R"c(
    struct gs;
    struct th { struct th *twups; struct gs *g; };
    struct gs { char *allgc; struct th *twups; };
    static void freeall(struct th *L) {
      struct gs *g = L->g;
      g->twups = L;
      free(g->twups->g->allgc);
    }
    void close_state(struct th *L) {
      L->g->twups = L;
      freeall(L);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(ResourceLifecycle, MemoryBelowAFreedObjectGoesWithItsContainer) {
  const auto result = analyze(R"c(
    struct node { struct node *child; char *s; };
    static void del(struct node *o) {
      if (o->child) del(o->child);
      free(o->s);
      free(o);
    }
    void twice(struct node *o) { del(o); del(o); }
    void in_loop(struct node *o, int k) { while (k--) del(o); }
  )c");
  ASSERT_TRUE(result.ast);
  // One report per object, not one per field the callee frees along with it.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: 'o' is freed twice", "9: 'o' is freed twice"}));
  // The recursive call names `o->child`; what `del` frees below that is not
  // this function's to describe, so the summary does not grow one level per
  // fixpoint iteration.
  const core::FunctionSummary *del = result.summary("del");
  ASSERT_NE(del, nullptr);
  std::vector<core::SummaryPath> consumed;
  for (const auto &[path, effect] : del->effects) {
    if (effect.consumed())
      consumed.push_back(path);
  }
  const core::SummaryPath o = core::SummaryPath::param(0);
  EXPECT_EQ(consumed, (std::vector<core::SummaryPath>{
                          o, o.deref().field("child"), o.deref().field("s")}));
}

TEST(ResourceLifecycle, ContainerCheckRunsForLibraryReleasesOnly) {
  const auto result = analyze(R"c(
    struct box { char *p; };
    static void box_free(struct box *b) { free(b); }
    void library(struct box *b) { b->p = malloc(8); free(b); }
    void defined(struct box *b) { b->p = malloc(8); box_free(b); }
  )c");
  ASSERT_TRUE(result.ast);
  // What a defined destructor does with the fields is checked where its own
  // `free` is; the caller does not second-guess its summary.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'b->p' is leaked when 'b' is freed"}));
}

TEST(ResourceLifecycle, ListBuildingLoopIsClean) {
  const auto result = analyze(R"c(
    struct item { struct item *next; struct item *prev; struct item *child; };
    static struct item *create(void) { return malloc(sizeof(struct item)); }
    static void link(struct item *prev, struct item *item) {
      prev->next = item;
      item->prev = prev;
    }
    struct item *build(int count) {
      int i;
      struct item *n = NULL;
      struct item *p = NULL;
      struct item *a = create();
      for (i = 0; a && i < count; i++) {
        n = create();
        if (!n) return a;
        if (!p) a->child = n; else link(p, n);
        p = n;
      }
      return a;
    }
  )c");
  ASSERT_TRUE(result.ast);
  // Path-insensitively `a->child = n` runs every iteration; the previous
  // node is still referenced from the list, through a pointer the analysis
  // has since forgotten (RFC 0007, *Escape*).
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(ResourceLifecycle, AnEscapedAliasMeansTheResourceEscaped) {
  // zlib's `gz_fetch`: `state->x.next = state->out` is done again after a
  // callee nobody can see took `state->out`. The one allocation escaped
  // through the alias; overwriting the other name loses nothing (RFC 0007,
  // *Escape*).
  const auto result = analyze(R"c(
    struct st { char *out; char *next; };
    void keep(char *p);
    void g(struct st *s, int c) {
      s->out = malloc(8);
      s->next = s->out;
      if (c) keep(s->out);
      s->next = s->out;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: call to 'keep' is not checked: it has no definition "
                     "or ownership annotations here"}));
}

TEST(ResourceLifecycle, AFreedElementDoesNotReleaseWhatAnotherElementHolds) {
  // linenoise's history: `free(history[0])` leaves a record on `*history`
  // witnessed by element 0; the store to `history[len]` is a different
  // element, so the alias keeps `linecopy` (RFC 0006, *Element witnesses*).
  const auto result = analyze(R"c(
    static char **history;
    static int history_len;
    int add(const char *line) {
      char *linecopy = strdup(line);
      if (!linecopy) return 0;
      free(history[0]);
      history[history_len] = linecopy;
      history_len++;
      return 1;
    }
    int add_first(const char *line) {
      char *linecopy = strdup(line);
      if (!linecopy) return 0;
      free(history[0]);
      history[0] = linecopy;
      return 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

// -- Outcome-conditional null facts (RFC 0007, *Acquiring and losing*) --------

TEST(ResourceLifecycle, OutParameterConstructorsPropagateNullOnFailure) {
  const auto result = analyze(R"c(
    static int make(char **out) {
      *out = malloc(8);
      return *out != NULL;
    }
    static int make_rc(char **out) {
      *out = malloc(8);
      if (*out == NULL) return -1;
      return 0;
    }
    void user(void) {
      char *s;
      if (!make(&s)) return;
      free(s);
    }
    void user_rc(void) {
      char *s;
      if (make_rc(&s) < 0) return;
      free(s);
    }
    void untested(void) {
      char *s;
      make(&s);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // An address-taken local dies at the scope's end; the report lands on the
  // statement the scope ends after, not on the declaration.
  EXPECT_EQ(messages(result.diagnostics), (Strings{"23: 's' is leaked"}));
  const core::FunctionSummary *make = result.summary("make");
  ASSERT_NE(make, nullptr);
  ASSERT_TRUE(make->nullOn.contains(core::Outcome::Zero));
  EXPECT_TRUE(make->nullOn.at(core::Outcome::Zero)
                  .contains(core::SummaryPath::param(0).deref()));
  const core::FunctionSummary *rc = result.summary("make_rc");
  ASSERT_NE(rc, nullptr);
  ASSERT_TRUE(rc->nullOn.contains(core::Outcome::Negative));
  EXPECT_TRUE(rc->nullOn.at(core::Outcome::Negative)
                  .contains(core::SummaryPath::param(0).deref()));
}

TEST(ResourceLifecycle, ACopyOfAConditionallyFreedPathIsNotTracked) {
  // zlib's `gz_look` / `gz_fetch`: the callee frees `state->out` on its
  // error path only and otherwise hands `state->x.next` a copy of it. The
  // copy is not a resource of its own (overwriting it loses nothing), and
  // not a dangling pointer either (`== -1` cannot retract the class).
  const auto result = analyze(R"c(
    struct gz { unsigned char *out; unsigned char *next; unsigned size; };
    static int look(struct gz *state) {
      state->out = malloc(32);
      if (state->out == NULL) { free(state->out); return -1; }
      state->next = state->out;
      return 0;
    }
    int fetch(struct gz *state) {
      if (look(state) == -1) return -1;
      state->next = state->out;
      return 0;
    }
    int comp(struct gz *state) {
      if (state->size == 0 && look(state) == -1) return -1;
      state->next[0] = 1;
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(ResourceLifecycle, StoresPastArgumentChecksDoNotHoldOnTheErrorClass) {
  // zlib's `deflateInit2_`: every `return Z_STREAM_ERROR` before the
  // allocation leaves `strm->state` as the caller had it, which is what the
  // caller has too; only the class that returns past the store holds it.
  const auto result = analyze(R"c(
    struct st { int a; };
    struct strm { struct st *state; int avail; };
    static int end(struct strm *s) {
      if (s == NULL || s->state == NULL) return -2;
      free(s->state);
      s->state = NULL;
      return 0;
    }
    static int init(struct strm *s, int level) {
      struct st *st;
      if (s == NULL) return -2;
      if (level < 0 || level > 9) return -2;
      st = malloc(sizeof *st);
      if (st == NULL) return -4;
      s->state = st;
      st->a = level;
      if (s->avail == 0) {
        end(s);
        return -4;
      }
      return 0;
    }
    int compress(int level) {
      struct strm stream;
      int err;
      stream.avail = level;
      err = init(&stream, level);
      if (err != 0) return err;
      end(&stream);
      return 0;
    }
    int leaks(int level) {
      struct strm stream;
      stream.avail = level;
      return init(&stream, level);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"36: 'stream.state' is leaked"}));
  const core::FunctionSummary *init = result.summary("init");
  ASSERT_NE(init, nullptr);
  EXPECT_FALSE(init->nullOn.contains(core::Outcome::Zero));
  ASSERT_TRUE(init->nullOn.contains(core::Outcome::Negative));
  EXPECT_TRUE(
      init->nullOn.at(core::Outcome::Negative)
          .contains(core::SummaryPath::param(0).deref().field("state")));
}

// -- Summaries carry families and reach callers ------------------------------

TEST(ResourceLifecycle, CalleeFreesWhatTheCallerAllocated) {
  const auto result = analyze(R"c(
    struct buf { char *data; };
    static void destroy(struct buf *b) { free(b->data); free(b); }
    static void reset(struct buf *b) { free(b->data); b->data = NULL; }
    void via_destroy(void) {
      struct buf *b = malloc(sizeof *b);
      if (!b) return;
      b->data = malloc(4);
      destroy(b);
    }
    void via_reset(struct buf *b) {
      b->data = malloc(4);
      reset(b);
      b->data = malloc(8);
      reset(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

} // namespace
} // namespace weavec::test
