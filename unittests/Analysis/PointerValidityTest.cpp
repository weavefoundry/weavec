//===- PointerValidityTest.cpp - Null, uninitialised, invalid releases ----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0008, *Pointer validity*: `null-dereference`, `use-of-uninitialized`,
// `invalid-release`, the `replaced` consume flag, `result`-rooted stores and
// the `WEAVEC_NULLABLE` / `WEAVEC_NONNULL` annotations.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using core::SummaryPath;
using weavec::test::analyze;
using weavec::test::ids;
using weavec::test::messages;
using weavec::test::notes;

using Strings = std::vector<std::string>;

/// Shared types; `#line 1` keeps the snippet's line numbers as `messages`
/// counts them (the raw string's opening newline is line 1).
constexpr const char *Types = R"c(
struct node { int v; struct node *next; char *name; };
struct buf { char *data; unsigned len; };
#line 1
)c";

// -- Null dereference ---------------------------------------------------------

TEST(NullDereference, UncheckedAllocatorResult) {
  const auto result = analyze(std::string(Types) + R"c(
    int f(void) {
      struct node *n = malloc(sizeof *n);
      n->v = 1;
      int r = n->v;
      free(n);
      return r;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: the result of 'malloc' is used without a null test; "
                     "it is null when allocation fails"}))
      << "one bad pointer reports once";
  EXPECT_EQ(ids(result.diagnostics), (Strings{"allocation-failure"}));
  EXPECT_EQ(notes(result.diagnostics), (Strings{"allocated here"}));
}

TEST(NullDereference, ConstantNull) {
  const auto result = analyze(R"c(
    int f(void) {
      int *p = NULL;
      return *p;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: dereference of 'p', which is null"}));
  EXPECT_EQ(notes(result.diagnostics), (Strings{"'p' is assigned NULL here"}));
}

TEST(NullDereference, TestsEstablishNonNull) {
  // Every recognised shape of test (RFC 0008, *Nullness*, `Tested`).
  const auto result = analyze(std::string(Types) + R"c(
    int early(void) {
      struct node *n = malloc(sizeof *n);
      if (!n) return 0;
      n->v = 1;
      free(n);
      return 0;
    }
    int equal(struct node *n) {
      if (n == NULL) return -1;
      return n->v;
    }
    int and_(struct node *n) { return n && n->v; }
    int or_(struct node *n) { return !n || n->v; }
    int ternary(struct node *n) { return n ? n->v : 0; }
    void walk(struct node *head) {
      for (struct node *n = head; n; n = n->next)
        n->v = 0;
    }
    void loop(struct node *n) {
      while (n != NULL) {
        n->v = 0;
        n = n->next;
      }
    }
    void not_equal(struct node *n) {
      if (n != NULL)
        n->v = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(NullDereference, TestedThenMergedIsMaybeNull) {
  // The null path did not end: after the merge the pointer may be null.
  const auto result = analyze(std::string(Types) + R"c(
    void warn(void);
    int f(struct node *n) {
      if (n == NULL) warn();
      return n->v;
    }
    void g(struct node *n) {
      while (n != NULL) n = n->next;
      n->v = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"9: dereference of 'n', which is null"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"'n' may be null: it is compared with NULL here"}));
}

TEST(NullDereference, RedundantTestsKeepANonNullFact) {
  // A pointer already known non-null and retested on every use (cJSON's
  // `can_access_at_index` macro): the null edge is infeasible and must not
  // make the pointer maybe-null once the edges merge (RFC 0008,
  // *Implementation notes*).
  const auto result = analyze(std::string(Types) + R"c(
    static unsigned skip(struct buf *b) {
      if (b == NULL || b->data == NULL) return 0;
      while (b != NULL && b->len > 0 && b->data[b->len - 1] == ' ') b->len--;
      return b->len;
    }
    static unsigned retest(struct buf *b) {
      if (!b) return 0;
      if (b == NULL) return 1;
      return b->len;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

TEST(NullDereference, ADereferenceEstablishesNonNull) {
  // The path could only have continued past `n->v` with a non-null `n`, so
  // a later test whose null edge merges back does not make it maybe-null
  // (cJSON's `buffer_at_offset(b)` at the top of a function, then
  // `cannot_access_at_index(b, 0)` further down). Passing it to a callee
  // that requires it counts as a dereference.
  const auto result = analyze(std::string(Types) + R"c(
    static int get(struct node *n) { return n->v; }
    int direct(struct node *n) {
      int v = n->v;
      if (n == NULL) v = 0;
      return n->v + v;
    }
    int via_callee(struct node *n) {
      int v = get(n);
      if (!n) v = 0;
      return n->v + v;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

TEST(NullDereference, CastNullConstantsAreNullTests) {
  // `(struct node *)0` is not a null pointer constant in ISO C's sense
  // (zlib's `buf != (charf *)0`), but it is null all the same.
  const auto result = analyze(std::string(Types) + R"c(
    static int get(struct node *n) { return n->v; }
    static int guarded(struct node *n, int c) {
      if (c && n != (struct node *)0) return get(n);
      return 0;
    }
    int f(void) {
      struct node *n = (struct node *)0;
      return n->v;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"9: dereference of 'n', which is null"}));
}

TEST(NullDereference, UncheckedCalleesMayWriteWhatTheyReach) {
  // A call into unchecked code may store through any pointer it is handed:
  // the facts below `&lc` and at `p` through `&p` are gone, the fact on a
  // pointer passed by value stays (linenoise's completion callback fills a
  // `linenoiseCompletions lc = { 0, NULL }`).
  const auto result = analyze(std::string(Types) + R"c(
    struct lc { unsigned len; char **cvec; };
    void fill(struct lc *out);
    void look(char *p);
    char by_address(void) {
      struct lc lc = { 0, NULL };
      fill(&lc);
      return lc.cvec[0][0];
    }
    char pointer_by_address(void) {
      char *p = NULL;
      fill((struct lc *)&p);
      return p[0];
    }
    char by_value(void) {
      char *p = NULL;
      look(p);
      return p[0];
    }
  )c");
  ASSERT_TRUE(result.ast);
  // RFC 0030 §5.1: the calls into unknown code report nothing; the value
  // passed by value keeps its nullness.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"18: dereference of 'p', which is null"}));
  EXPECT_EQ(
      test::unknownCalls(result),
      (Strings{"7: fill(&lc)", "12: fill((structlc*)&p)", "17: look(p)"}));
}

TEST(NullDereference, UnknownPointersAreTrusted) {
  // A parameter, a loaded field, the result of unannotated code: nothing is
  // known, nothing is reported (RFC 0008, *Bugs deliberately not caught*).
  const auto result = analyze(std::string(Types) + R"c(
    struct node *lookup(int);
    int param(struct node *n) { return n->v; }
    int field(struct node *n) { return n->next->v; }
    int external(void) { return lookup(1)->v; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
  // RFC 0030 §5.1: the call to `lookup` is into unknown code.
  EXPECT_EQ(test::unknownCalls(result), (Strings{"5: lookup(1)"}));
}

TEST(NullDereference, DereferencesBecomeRequirements) {
  const auto result = analyze(std::string(Types) + R"c(
    static int get(struct node *n) { return n->v; }
    static int tolerant(struct node *n) { return n ? n->v : 0; }
    static int later(struct node *n, int c) { if (c) return 0; return n->v; }
    int via_copy(struct node *n) { struct node *m = n; return m->v; }
    void pass_null(void) { get(NULL); }
    void pass_maybe(void) {
      struct node *n = malloc(sizeof *n);
      get(n);
      free(n);
    }
    void pass_tolerant(void) { tolerant(NULL); }
    void pass_checked(struct node *n) { if (n) get(n); }
    void pass_unknown(struct node *n) { get(n); }
  )c");
  ASSERT_TRUE(result.ast);
  // RFC 0030 §7.5: `get` is static and must-accesses `n`, so every direct
  // call checks its argument; a null one is the call's violation. (The
  // summary's `requires` stays a may-fact, `later`'s included.)
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: a null pointer is passed to 'get', which "
                     "dereferences it"}));
}

TEST(NullDereference, CalleeResultsCarryNullness) {
  const auto result = analyze(std::string(Types) + R"c(
    void abort(void) __attribute__((noreturn));
    static struct node *make(void) {
      struct node *n = malloc(sizeof *n);
      if (n) n->v = 0;
      return n;
    }
    static struct node *make_or_die(void) {
      struct node *n = malloc(sizeof *n);
      if (!n) abort();
      return n;
    }
    int f(void) {
      struct node *n = make();
      int v = n->v;
      free(n);
      return v;
    }
    int g(void) {
      struct node *n = make_or_die();
      int v = n->v;
      free(n);
      return v;
    }
    int h(void) { return make()->v; }
  )c");
  ASSERT_TRUE(result.ast);
  // RFC 0031 §5.8: `h` drops the object `make` returns fresh, a leak the
  // object engine follows the result to.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"15: the result of 'make' is used without a null test; it "
                     "is null when allocation fails",
                     "25: the result of 'make' is used without a null test; it "
                     "is null when allocation fails",
                     "25: result of 'make' is leaked"}))
      << "the direct dereference of a call result is checked too";
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"allocated here"}));
}

TEST(NullDereference, TableEntries) {
  const auto result = analyze(R"c(
    char *strchr(const char *, int);
    void *memcpy(void *, const void *, size_t);
    typedef struct FILE FILE;
    int fclose(FILE *);
    void found(const char *s) {
      char *c = strchr(s, 'x');
      *c = 0;
    }
    void checked(const char *s) {
      char *c = strchr(s, 'x');
      if (c) *c = 0;
    }
    void copy(void) {
      char *d = malloc(8);
      memcpy(d, "abc", 4);
      free(d);
    }
    void close_null(void) { fclose(NULL); }
    void free_null(void) { free(NULL); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "16: the result of 'malloc' is used without a null test; it is null "
          "when allocation fails",
          "19: a null pointer is passed to 'fclose', which dereferences it"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"allocated here"}));
}

TEST(NullDereference, NullnessFollowsTheValue) {
  const auto result = analyze(std::string(Types) + R"c(
    void field(struct node *n) {
      n->next = NULL;
      n->next->v = 1;
    }
    void copy(void) {
      struct node *n = malloc(sizeof *n);
      struct node *m = n;
      m->v = 1;
      free(n);
    }
    void reassigned(struct node *other) {
      struct node *n = malloc(sizeof *n);
      free(n);
      n = other;
      n->v = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: dereference of 'n->next', which is null",
                     "9: the result of 'malloc' is used without a null test; "
                     "it is null when allocation fails"}));
}

// -- Annotations --------------------------------------------------------------

TEST(NullDereference, AnnotationsOnUncheckedCallees) {
  // RFC 0008, *Annotation surface*: neither annotation says anything about
  // ownership, so an otherwise unannotated declaration is still an unknown
  // callee (RFC 0030 §5.1), but what they do say holds. RFC 0030 §3.2: a
  // result that may be null is a checked facet; a null argument where the
  // declaration requires non-null is definite.
  const auto result = analyze(std::string(Types) + R"c(
    char *NULLABLE lookup(int k);
    void need(struct node *NONNULL n);
    int result(void) { return lookup(1)[0]; }
    void argument(void) { need(NULL); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: a null pointer is passed to 'need', which "
                     "dereferences it"}));
  EXPECT_EQ(test::unknownCalls(result),
            (Strings{"4: lookup(1)", "5: need(NULL)"}));
}

TEST(NullDereference, ContradictoryAnnotationsAreInvalid) {
  const auto result = analyze(std::string(Types) + R"c(
    int f(struct node *NULLABLE NONNULL n) { return 0; }
    char *NULLABLE NONNULL g(void) { return 0; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"2: 'n' is declared both WEAVEC_NULLABLE and WEAVEC_NONNULL",
               "3: 'g' is declared both WEAVEC_NULLABLE and "
               "WEAVEC_NONNULL"}));
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"invalid-annotation", "invalid-annotation"}));
}

// -- Uninitialised pointers ---------------------------------------------------

TEST(UseOfUninitialized, LocalsAndFields) {
  const auto result = analyze(std::string(Types) + R"c(
    struct outer { struct buf inner; char *q; };
    void read(void) { char *p; use(p); }
    void deref(void) { int *p; *p = 1; }
    void copy(void) { char *p; char *q = p; use(q); }
    void field(void) { struct buf b; use(b.data); }
    void nested(void) { struct outer o; use(o.inner.data); }
    void release(void) { char *p; free(p); }
    void maybe(int c) { char *p; if (c) p = malloc(4); use(p); free(p); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"3: use of 'p' before it was initialized",
                     "4: use of 'p' before it was initialized",
                     "5: use of 'p' before it was initialized",
                     "6: use of 'b.data' before it was initialized",
                     "7: use of 'o.inner.data' before it was initialized",
                     "8: use of 'p' before it was initialized"}))
      << "a copy reports at the copy, once; a may-uninitialised value is "
         "made defined by zero-initialisation (RFC 0030 §3.1)";
  EXPECT_EQ(ids(result.diagnostics)[0], "use-of-uninitialized");
  EXPECT_EQ(notes(result.diagnostics), (Strings{"'p' is declared here"}));
}

TEST(UseOfUninitialized, InitialisationSilences) {
  // (RFC 0031 §5.4: a callee writes only what it reaches through non-const
  // pointees; `use(&p)` hands `p` over as `const void *` and leaves it
  // uninitialised, so the address is taken by a mutable borrow here.)
  const auto result = analyze(std::string(Types) + R"c(
    void fill(struct buf *MUT);
    void init(void) { char *p = NULL; use(p); }
    void assigned(void) { char *p; p = malloc(4); use(p); free(p); }
    void out(void) { struct buf b; fill(&b); use(b.data); }
    void address_taken(void) { char *p; poke(&p); use(p); }
    void field_set(void) { struct buf b; b.data = NULL; use(b.data); }
    void static_(void) { static char *p; use(p); }
    void zeroed(void) { struct buf b = {0}; use(b.data); }
    void integers(void) { int n; struct buf b; b.len = 1; use(&b); }
    void all_paths(int c) { char *p; if (c) p = NULL; else p = malloc(4); use(p); free(p); }
    void loop(void) { char *p; for (int i = 0; i < 3; i++) { p = malloc(4); free(p); } }
    void whole(struct buf src) { struct buf b; b = src; use(b.data); }
    void array(void) { char *a[2]; a[0] = NULL; use(a[0]); }
    typedef __builtin_va_list va_list;
    void variadic(const char *fmt, ...) {
      va_list ap, cpy;
      __builtin_va_start(ap, fmt);
      __builtin_va_copy(cpy, ap);
      __builtin_va_end(cpy);
      __builtin_va_end(ap);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

// -- Invalid releases ---------------------------------------------------------

TEST(InvalidRelease, NonHeapObjects) {
  const auto result = analyze(std::string(Types) + R"c(
    typedef struct FILE FILE;
    int fclose(FILE *);
    static char g_buf[16];
    static struct buf g_b;
    void stack(void) { char buf[8]; free(buf); }
    void address(void) { int x; free(&x); }
    void field(void) { struct buf b; free(&b.len); }
    void literal(void) { free("abc"); }
    void global(void) { free(g_buf); }
    void global_struct(void) { free(&g_b); }
    void wrong_family(void) { struct buf b; fclose((FILE *)&b); }
    void via_copy(void) { char buf[8]; char *p = buf; free(p); }
    void via_literal(void) { const char *s = "abc"; free((char *)s); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"6: 'buf' is released but is not a heap object",
               "7: 'x' is released but is not a heap object",
               "8: 'b.len' is released but is not a heap object",
               "9: a string literal is released",
               "10: 'g_buf' is released but is not a heap object",
               "11: 'g_b' is released but is not a heap object",
               "12: 'b' is released but is not a heap object",
               "13: 'p' is released but points to 'buf', which is not a heap "
               "object",
               "14: 's' is released but points to a string literal"}));
  EXPECT_EQ(ids(result.diagnostics)[0], "invalid-release");
  EXPECT_EQ(notes(result.diagnostics), (Strings{"'buf' is declared here"}));
  EXPECT_EQ(notes(result.diagnostics, 7), (Strings{"'buf' is declared here"}));
}

TEST(InvalidRelease, InteriorPointers) {
  const auto result = analyze(R"c(
    char *strchr(const char *, int);
    void offset(void) { char *p = malloc(8); if (!p) return; free(p + 1); }
    void alias(void) { char *p = malloc(8); if (!p) return; char *q = p + 1; free(q); }
    void incremented(void) { char *p = malloc(8); if (!p) return; p++; free(p); }
    void found(void) {
      char *p = malloc(8);
      if (!p) return;
      char *q = strchr(p, 'x');
      if (q) free(q);
    }
    void zero(void) { char *p = malloc(8); if (!p) return; free(p + 0); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"3: 'p' is released but points 1 element past the start of its "
               "allocation",
               "4: 'q' is released but points 1 element past the start of its "
               "allocation",
               "5: 'p' is released but points 1 element past the start of its "
               "allocation",
               "10: 'q' is released but may not point to the start of its "
               "allocation"}))
      << "`p + 0` is `p`; a known offset is named (RFC 0011)";
  EXPECT_EQ(notes(result.diagnostics), (Strings{"allocated here"}));
}

TEST(InvalidRelease, ValidReleasesAreClean) {
  const auto result = analyze(std::string(Types) + R"c(
    void heap(void) { char *p = malloc(8); free(p); }
    void alias(void) { char *p = malloc(8); char *q = p; free(q); }
    void param(char *OWNED p) { free(p); }
    void field(struct buf *b) { free(b->data); }
    void element(char **arr) { free(arr[0]); }
    void conditional(int c) { char *p = c ? malloc(8) : NULL; free(p); }
    void pick(char *OWNED a, char *OWNED b, int c) { free(c ? a : b); free(c ? b : a); }
    void strchr_of_param(char *s) { char *strchr(const char *, int); free(strchr(s, 'x')); }
  )c");
  ASSERT_TRUE(result.ast);
  // `pick` frees both on one path and neither on the other: RFC 0007 leaks
  // aside, no invalid release is reported.
  for (const core::Diagnostic &d : result.diagnostics.diagnostics())
    EXPECT_NE(d.id, core::diag::InvalidRelease) << d.message;
}

TEST(InvalidRelease, AFieldOfAParameterIsNotAnAllocation) {
  // RFC 0031 §5.5 and A3: `b` points to a whole `struct buf`, so `&b->len`
  // lies 8 bytes into the allocation that holds it, never at its start.
  // (Before the object engine a caller's pointer had no known offset and
  // this release was accepted.)
  const auto result = analyze(std::string(Types) + R"c(
    void interior_of_unknown(struct buf *b) { free(&b->len); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"2: 'b' is released but points to field 'len' of its "
                     "allocation"}));
  EXPECT_EQ(ids(result.diagnostics), (Strings{"invalid-release"}));
}

TEST(InvalidRelease, DoesNotRepeatForTheSameStorage) {
  // Clang's -Wfree-nonheap-object is separate; ours is once per release.
  const auto result = analyze(R"c(
    void f(int c) {
      char buf[8];
      char *p = buf;
      if (c) p = malloc(8);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: 'p' is released but may point to 'buf', which is not "
                     "a heap object"}))
      << "may point at the stack: reported";
}

// -- Replaced values (the RFC 0003 soundness hole) ---------------------------

// -- Struct-by-value results --------------------------------------------------

// -- Crash regression ---------------------------------------------------------

TEST(InvalidRelease, FreeingAStaticArrayDoesNotCrash) {
  const auto result = analyze(R"c(
    static char table[4][8];
    void f(void) { free(table); }
    void g(void) { free(table[1]); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"invalid-release", "invalid-release"}));
}

} // namespace
} // namespace weavec::analysis
