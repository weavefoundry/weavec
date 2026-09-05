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
using core::ValueSource;
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
            (Strings{"4: dereference of 'n', which may be null"}))
      << "one bad pointer reports once";
  EXPECT_EQ(ids(result.diagnostics), (Strings{"null-dereference"}));
  EXPECT_EQ(notes(result.diagnostics),
            (Strings{"'n' may be null: it is the result of 'malloc' here"}));
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
            (Strings{"5: dereference of 'n', which may be null",
                     "9: dereference of 'n', which is null"}));
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
  EXPECT_FALSE(result.summary("skip")->requiresParam(0));
  EXPECT_FALSE(result.summary("retest")->requiresParam(0));
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
  EXPECT_TRUE(result.summary("direct")->requiresParam(0));
  EXPECT_TRUE(result.summary("via_callee")->requiresParam(0));
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
  EXPECT_FALSE(result.summary("guarded")->requiresParam(0));
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
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"annotation-required", "annotation-required",
                     "null-dereference"}));
  EXPECT_EQ(messages(result.diagnostics)[2],
            "18: dereference of 'p', which is null");
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
  EXPECT_EQ(ids(result.diagnostics), (Strings{"annotation-required"}))
      << "only the RFC 0003 boundary warning";
}

TEST(NullDereference, DereferencesBecomeRequirements) {
  const auto result = analyze(std::string(Types) + R"c(
    static int get(struct node *n) { return n->v; }
    static int tolerant(struct node *n) { return n ? n->v : 0; }
    static int later(struct node *n, int c) { if (c) return 0; return n->v; }
    static int via_copy(struct node *n) { struct node *m = n; return m->v; }
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
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: a null pointer is passed to 'get', which dereferences "
                     "it",
                     "9: 'n', which may be null, is passed to 'get', which "
                     "dereferences it"}));
  EXPECT_EQ(notes(result.diagnostics, 1),
            (Strings{"'n' may be null: it is the result of 'malloc' here",
                     "'get' is declared here"}));
  EXPECT_TRUE(result.summary("get")->requiresParam(0));
  EXPECT_FALSE(result.summary("tolerant")->requiresParam(0));
  EXPECT_TRUE(result.summary("later")->requiresParam(0))
      << "a may-fact like every other effect";
  EXPECT_TRUE(result.summary("via_copy")->requiresParam(0));
  EXPECT_TRUE(result.summary("pass_unknown")->requiresParam(0))
      << "requirements propagate through callers";
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
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"15: dereference of 'n', which may be null",
                     "25: dereference of the result of 'make', which may be "
                     "null"}))
      << "the direct dereference of a call result is checked too";
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"'n' may be null: it is the result of 'make' here"}));
  EXPECT_TRUE(result.summary("make")->mayReturnNull());
  EXPECT_FALSE(result.summary("make_or_die")->mayReturnNull())
      << "the null path ends in the wrapper";
}

TEST(NullDereference, OutParametersFollowTheOutcome) {
  // RFC 0008, *Per-outcome non-null facts*.
  const auto result = analyze(std::string(Types) + R"c(
    static int make(struct node **out) {
      *out = malloc(sizeof **out);
      return *out != NULL;
    }
    int tested(void) {
      struct node *n;
      if (!make(&n)) return 0;
      int v = n->v;
      free(n);
      return v;
    }
    int untested(void) {
      struct node *n;
      make(&n);
      int v = n->v;
      free(n);
      return v;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"16: dereference of 'n', which may be null"}));
  EXPECT_EQ(notes(result.diagnostics),
            (Strings{"'n' may be null: it is set by 'make' here"}));
  const core::FunctionSummary *make = result.summary("make");
  ASSERT_NE(make, nullptr);
  EXPECT_EQ(make->nonNullOn.at(core::Outcome::Positive),
            std::set<SummaryPath>{SummaryPath::param(0).deref()});
  EXPECT_EQ(make->nullOn.at(core::Outcome::Zero),
            std::set<SummaryPath>{SummaryPath::param(0).deref()});
}

TEST(NullDereference, AStoreThatDidNotHappenIsNotANullFact) {
  // RFC 0007 puts a `fresh` store's destination into `nullOn` for a class
  // on which the store did not take effect, so the caller retracts the
  // record. It is not a nullness fact: on the failing class the caller's
  // memory holds what it held before (RFC 0008, *Implementation notes*).
  const auto result = analyze(std::string(Types) + R"c(
    static int grow(struct buf *b, unsigned n) {
      if (n <= b->len) return 0;
      char *p = realloc(b->data, n);
      if (p == NULL) return -1;
      b->data = p;
      b->len = n;
      return 0;
    }
    void append(struct buf *b, char c) {
      if (grow(b, b->len + 1) == -1) return;
      b->data[b->len - 1] = c;
    }
    void truncate(struct buf *b, unsigned n) {
      if (grow(b, n) == -1 && n > b->len) n = b->len;
      b->data[0] = 0;
    }
    static int make(struct node **out) {
      *out = malloc(sizeof **out);
      if (*out == NULL) return -1;
      return 0;
    }
    int on_failure(void) {
      struct node *n;
      if (make(&n) != 0) return n->v;
      free(n);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"25: dereference of 'n', which is null"}))
      << "`make` stores `{fresh, null}`: its failing class is null; `grow` "
         "stores only `fresh`: its failing class stored nothing";
  EXPECT_EQ(notes(result.diagnostics),
            (Strings{"'n' may be null: it is set by 'make' here"}));
  const core::FunctionSummary *grow = result.summary("grow");
  ASSERT_NE(grow, nullptr);
  EXPECT_EQ(grow->nullOn.at(core::Outcome::Negative),
            std::set<SummaryPath>{SummaryPath::param(0).deref().field("data")})
      << "the RFC 0007 relaxation still lets the caller drop the record";
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
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: dereference of 'c', which may be null",
                     "16: 'd', which may be null, is passed to 'memcpy', "
                     "which dereferences it",
                     "19: a null pointer is passed to 'fclose', which "
                     "dereferences it"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"'c' may be null: it is the result of 'strchr' here"}));
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
                     "9: dereference of 'm', which may be null"}));
}

// -- Annotations --------------------------------------------------------------

TEST(NullDereference, NullableAnnotation) {
  const auto result = analyze(std::string(Types) + R"c(
    int body(struct node *NULLABLE n) { return n->v; }
    int body_checked(struct node *NULLABLE n) { return n ? n->v : 0; }
    void caller(struct node *NULLABLE n) { body_checked(n); body(NULL); }
    static const char *NULLABLE lookup(int k) { return k ? "x" : "y"; }
    int result(void) { return lookup(1)[0]; }
    int result_local(void) { const char *p = lookup(1); return *p; }
    int result_checked(void) { const char *p = lookup(1); return p ? *p : 0; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"2: dereference of 'n', which may be null",
                     "6: dereference of the result of 'lookup', which may be "
                     "null",
                     "7: dereference of 'p', which may be null"}))
      << "a nullable parameter imposes nothing on callers; a nullable result "
         "is nullable whatever the body says";
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"'n' is declared WEAVEC_NULLABLE here"}));
  EXPECT_EQ(notes(result.diagnostics, 2),
            (Strings{"'p' may be null: the result of 'lookup' is declared "
                     "WEAVEC_NULLABLE here"}));
  EXPECT_FALSE(result.summary("body")->requiresParam(0))
      << "the body is checked instead";
  const auto lookup =
      result.analyzer->summaries().lookup(*result.function("lookup"));
  ASSERT_TRUE(lookup);
  EXPECT_TRUE(lookup->summary->mayReturnNull());
}

TEST(NullDereference, AnnotationsOnUncheckedCallees) {
  // RFC 0008, *Annotation surface*: neither annotation says anything about
  // ownership, so an otherwise unannotated declaration is still a boundary
  // (RFC 0003), but what they do say holds.
  const auto result = analyze(std::string(Types) + R"c(
    char *NULLABLE lookup(int k);
    void need(struct node *NONNULL n);
    int result(void) { return lookup(1)[0]; }
    void argument(void) { need(NULL); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"annotation-required", "null-dereference",
                     "annotation-required", "null-dereference"}));
  EXPECT_EQ(messages(result.diagnostics)[1],
            "4: dereference of the result of 'lookup', which may be null");
  EXPECT_EQ(notes(result.diagnostics, 1),
            (Strings{"the result of 'lookup' is declared WEAVEC_NULLABLE "
                     "here"}));
  EXPECT_EQ(messages(result.diagnostics)[3],
            "5: a null pointer is passed to 'need', which dereferences it");
}

TEST(NullDereference, NonNullAnnotation) {
  const auto result = analyze(std::string(Types) + R"c(
    static void need(struct node *NONNULL n) { if (n) n->v = 1; }
    static struct node *NONNULL wrapped(void) { return malloc(sizeof(struct node)); }
    void caller(void) {
      need(NULL);
      struct node *n = malloc(sizeof *n);
      need(n);
      free(n);
    }
    int result(void) { return wrapped()->v; }
    int result_local(void) { struct node *n = wrapped(); int v = n->v; free(n); return v; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: a null pointer is passed to 'need', which "
                     "dereferences it",
                     "7: 'n', which may be null, is passed to 'need', which "
                     "dereferences it"}))
      << "a NONNULL parameter is required even when the body tests it; a "
         "NONNULL result is trusted even when the body says otherwise";
  const auto wrapped =
      result.analyzer->summaries().lookup(*result.function("wrapped"));
  ASSERT_TRUE(wrapped);
  EXPECT_FALSE(wrapped->summary->mayReturnNull());
  EXPECT_EQ(result.diagnostics.diagnostics()[0].id,
            core::diag::NullDereference);
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
                     "8: use of 'p' before it was initialized",
                     "9: use of 'p' before it was initialized",
                     "9: use of 'p' before it was initialized"}))
      << "a copy reports at the copy, once; a may-uninitialised value "
         "reports";
  EXPECT_EQ(ids(result.diagnostics)[0], "use-of-uninitialized");
  EXPECT_EQ(notes(result.diagnostics), (Strings{"'p' is declared here"}));
}

TEST(UseOfUninitialized, InitialisationSilences) {
  const auto result = analyze(std::string(Types) + R"c(
    void fill(struct buf *MUT);
    void init(void) { char *p = NULL; use(p); }
    void assigned(void) { char *p; p = malloc(4); use(p); free(p); }
    void out(void) { struct buf b; fill(&b); use(b.data); }
    void address_taken(void) { char *p; use(&p); use(p); }
    void field_set(void) { struct buf b; b.data = NULL; use(b.data); }
    void static_(void) { static char *p; use(p); }
    void zeroed(void) { struct buf b = {0}; use(b.data); }
    void integers(void) { int n; struct buf b; b.len = 1; use(&b); }
    void all_paths(int c) { char *p; if (c) p = NULL; else p = malloc(4); use(p); free(p); }
    void loop(void) { char *p; for (int i = 0; i < 3; i++) { p = malloc(4); free(p); } }
    void whole(struct buf src) { struct buf b; b = src; use(b.data); }
    void array(void) { char *a[2]; a[0] = NULL; use(a[1]); }
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
               "10: 'q' is released but does not point to the start of its "
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
    void interior_of_unknown(struct buf *b) { free(&b->len); }
    void strchr_of_param(char *s) { char *strchr(const char *, int); free(strchr(s, 'x')); }
  )c");
  ASSERT_TRUE(result.ast);
  // `pick` frees both on one path and neither on the other: RFC 0007 leaks
  // aside, no invalid release is reported.
  for (const core::Diagnostic &d : result.diagnostics.diagnostics())
    EXPECT_NE(d.id, core::diag::InvalidRelease) << d.message;
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
            (Strings{"6: 'p' is released but points to 'buf', which is not a "
                     "heap object"}))
      << "may point at the stack: reported";
}

// -- Replaced values (the RFC 0003 soundness hole) ---------------------------

TEST(ReplacedValues, ConsumeThenOverwriteIsStillAConsume) {
  const auto result = analyze(R"c(
    struct vec { char *items; unsigned cap; };
    static void grow(struct vec *v) {
      char *bigger = realloc(v->items, v->cap * 2);
      if (!bigger) return;
      v->items = bigger;
      v->cap *= 2;
    }
    void f(struct vec *v) {
      char *old = v->items;
      grow(v);
      use(old);
      use(v->items);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"12: use of 'old' after it was moved"}))
      << "the copy of the old items is dead; the field itself holds the "
         "replacement";
  const core::FunctionSummary *grow = result.summary("grow");
  ASSERT_NE(grow, nullptr);
  const core::PlaceEffect items =
      grow->effectOf(SummaryPath::param(0).deref().field("items"));
  EXPECT_TRUE(items.moved);
  EXPECT_TRUE(items.replaced)
      << "on every path that consumed it the place was reinitialised; the "
         "failure path consumed nothing (RFC 0008, the realloc row)";
}

TEST(ReplacedValues, UnconditionalReplacement) {
  const auto result = analyze(R"c(
    struct buf { char *data; };
    static void reset(struct buf *b) { free(b->data); b->data = malloc(8); }
    static void drop(struct buf *b) { free(b->data); b->data = NULL; }
    static void clear(struct buf *b) { free(b->data); }
    void f(struct buf *b) {
      char *old = b->data;
      reset(b);
      use(old);
      use(b->data);
    }
    void g(struct buf *b) {
      drop(b);
      use(b->data);
      clear(b);
      use(b->data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"9: use of 'old' after it was freed",
                     "16: use of 'b->data' after it was freed"}));
  EXPECT_TRUE(result.summary("reset")
                  ->effectOf(SummaryPath::param(0).deref().field("data"))
                  .replaced);
  EXPECT_TRUE(result.summary("drop")
                  ->effectOf(SummaryPath::param(0).deref().field("data"))
                  .replaced);
  EXPECT_FALSE(result.summary("clear")
                   ->effectOf(SummaryPath::param(0).deref().field("data"))
                   .replaced);
}

TEST(ReplacedValues, OverwrittenPathsAreNotTheCallersValue) {
  // `b->data = malloc(n); free(b->data);` releases this function's value,
  // not the caller's (RFC 0008, *Replaced values*).
  const auto result = analyze(R"c(
    struct buf { char *data; };
    static void own_then_free(struct buf *b) {
      b->data = malloc(8);
      free(b->data);
    }
    void f(struct buf *b) {
      own_then_free(b);
      use(b->data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  const core::FunctionSummary *s = result.summary("own_then_free");
  ASSERT_NE(s, nullptr);
  const core::PlaceEffect data =
      s->effectOf(SummaryPath::param(0).deref().field("data"));
  EXPECT_TRUE(data.written);
  EXPECT_FALSE(data.freed) << "the caller's value was overwritten, not freed";
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(ReplacedValues, OwnValueSurvivesTheJoinWithAnUntouchedPath) {
  // zlib's `gz_look`: the allocation, the free and the error return are all
  // inside `if (state->size == 0)`. The exit state joins that path (moved,
  // overwritten) with the untouched one (neither); the join must not
  // manufacture "the caller's value was freed" (RFC 0008, *Replaced
  // values*, implementation notes), or every caller in a loop is a
  // `double-free`.
  const auto result = analyze(R"c(
    struct st { char *in; unsigned want, size; int how; };
    static int look(struct st *state) {
      if (state->size == 0) {
        state->in = malloc(state->want);
        if (state->in == NULL)
          return -1;
        if (state->want == 3) {
          free(state->in);
          return -1;
        }
        state->size = state->want;
      }
      return 0;
    }
    int fetch(struct st *state) {
      do {
        if (look(state) == -1)
          return -1;
      } while (state->how);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  const core::FunctionSummary *s = result.summary("look");
  ASSERT_NE(s, nullptr);
  const core::PlaceEffect in =
      s->effectOf(SummaryPath::param(0).deref().field("in"));
  EXPECT_TRUE(in.written);
  EXPECT_FALSE(in.freed) << "only this function's own value was freed";
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(ReplacedValues, ConsumptionOnAPathThatNeverReturnsIsNoEffect) {
  // Lua's `os_exit`: `lua_close(L)` frees `L->l_G`, then `exit()`. The
  // consumption is recorded as it happens (RFC 0008), but a path that never
  // hands control back is no part of what a call does (RFC 0003, *What a
  // summary describes*); the caller's `L->g` is intact. A parameter root
  // stays an event: `free(p); p = NULL;` frees the argument for good.
  const auto result = analyze(R"c(
    struct G { int n; };
    struct L { struct G *g; };
    static void close_state(struct L *L) { free(L->g); }
    static int os_exit(struct L *L, int flag) {
      if (flag) close_state(L);
      if (L) exit(1);
      return 0;
    }
    static void free_and_exit(char *p) { free(p); p = NULL; exit(1); }
    static void free_and_null(char *p) { free(p); p = NULL; }
    int caller(struct L *L, char *p, char *q) {
      os_exit(L, 0);
      free_and_null(p);
      use(p);
      free_and_exit(q);
      return L->g->n;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"15: use of 'p' after it was freed"}));
  const core::FunctionSummary *exitSummary = result.summary("os_exit");
  ASSERT_NE(exitSummary, nullptr);
  EXPECT_FALSE(exitSummary->effectOf(SummaryPath::param(0).deref().field("g"))
                   .consumed());
  EXPECT_TRUE(result.summary("close_state")
                  ->effectOf(SummaryPath::param(0).deref().field("g"))
                  .freed);
  EXPECT_TRUE(result.summary("free_and_null")->frees(0));
  EXPECT_FALSE(result.summary("free_and_null")
                   ->effectOf(SummaryPath::param(0))
                   .replaced);
  EXPECT_FALSE(result.summary("free_and_exit")->frees(0));
}

TEST(ReplacedValues, ElementConsumesApplyWithAnUnknownWitness) {
  // linenoise's `linenoiseEditFeed`: on ENTER it frees `history[len]` and
  // returns; the blocking wrapper calls it in a loop. Which element the
  // callee freed is not the caller's to know, so the second call is not a
  // `double-free` (RFC 0008, *Element consumes*; RFC 0006, *Element
  // witnesses*). A callee that frees the whole pointee still is.
  const auto result = analyze(R"c(
    static char **history;
    static int history_len;
    static char *more;
    static char *feed(char *buf, int c) {
      if (c == 13) {
        history_len--;
        free(history[history_len]);
        return strdup(buf);
      }
      return more;
    }
    char *loop(char *buf) {
      char *res;
      while ((res = feed(buf, buf[0])) == more);
      return res;
    }
    void twice(int c) { feed("a", c); feed("b", c); }
    static void drop_all(char **table) { free(*table); }
    void whole(char **table) { drop_all(table); drop_all(table); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"20: '*table' is freed twice"}));
  const core::FunctionSummary *feed = result.summary("feed");
  ASSERT_NE(feed, nullptr);
  bool sawHistory = false;
  for (const auto &[path, effect] : feed->effects) {
    if (!path.isGlobal() || !effect.consumed())
      continue;
    sawHistory = true;
    EXPECT_TRUE(effect.freed);
    EXPECT_TRUE(effect.element);
  }
  EXPECT_TRUE(sawHistory);
  EXPECT_FALSE(result.summary("drop_all")
                   ->effectOf(SummaryPath::param(0).deref())
                   .element);
}

// -- Struct-by-value results --------------------------------------------------

TEST(ResultStores, FieldsOfAReturnedStructAreTracked) {
  const auto result = analyze(R"c(
    struct pair { char *a; char *b; };
    static struct pair make(char *OWNED x) {
      struct pair p;
      p.a = malloc(4);
      p.b = x;
      return p;
    }
    void f(char *OWNED x) {
      struct pair p = make(x);
      free(p.a);
      free(p.b);
    }
    void leak(char *OWNED x) {
      struct pair p = make(x);
      free(p.b);
    }
    void twice(char *OWNED x) {
      struct pair p = make(x);
      free(p.b);
      free(x);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"16: 'p.a' is leaked", "21: 'p.a' is leaked",
                     "21: use of 'x' after it was moved"}));
  const core::FunctionSummary *make = result.summary("make");
  ASSERT_NE(make, nullptr);
  EXPECT_TRUE(make->storesTo(SummaryPath::result().field("a")));
  EXPECT_TRUE(make->stores.contains(core::Store{
      .dest = SummaryPath::result().field("a"),
      .value =
          ValueSource::freshAt("free", {}, core::PathAffine::ofConstant(4))}));
  EXPECT_TRUE(make->stores.contains(
      core::Store{.dest = SummaryPath::result().field("b"),
                  .value = ValueSource::copy(SummaryPath::param(0))}));
}

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
