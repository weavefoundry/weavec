//===- DataflowTest.cpp - Tests for the intra-procedural dataflow ---------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One test per behaviour promised by RFC 0002, grouped by section, plus the
// idioms that must stay clean. Line numbers in expectations count from the
// line after `R"c(`, which is line 1 (see TestUtils.h).
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/FunctionAnalysis.h"

#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using weavec::test::analyze;
using weavec::test::ids;
using weavec::test::messages;
using weavec::test::notes;

using Strings = std::vector<std::string>;

// -- Path sensitivity (RFC 0002, "Soundness examples") ------------------------

TEST(Dataflow, LoopBackEdgeExposesUseAndDoubleFree) {
  const auto result = analyze(R"c(
    void f(int n) {
      char *p = malloc(8);
      for (int i = 0; i < n; ++i) {
        p[0] = 0;
        free(p);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"5: use of 'p' after it was freed", "6: 'p' is freed twice"}));
}

TEST(Dataflow, DoWhileBackEdge) {
  const auto result = analyze(R"c(
    void f(int n) {
      char *p = malloc(4);
      do {
        use(p);
        free(p);
      } while (n--);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{std::string(core::diag::UseAfterFree),
                     std::string(core::diag::DoubleFree)}));
}

TEST(Dataflow, GotoBackEdge) {
  const auto result = analyze(R"c(
    void f(int n) {
      char *p = malloc(4);
    again:
      free(p);
      if (n--) goto again;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), (Strings{"5: 'p' is freed twice"}));
}

TEST(Dataflow, SwitchFallthrough) {
  const auto result = analyze(R"c(
    void f(int c) {
      char *p = malloc(8);
      switch (c) {
      case 0: free(p);
      case 1: free(p);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  // No default: `p` is lost on the edge that skips both cases (RFC 0007).
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'p' is leaked", "6: 'p' is freed twice"}));
}

TEST(Dataflow, ShortCircuitOperands) {
  const auto result = analyze(R"c(
    void f(int c) {
      char *p = malloc(4);
      if (c && (free(p), 1)) {}
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: use of 'p' after it was freed"}));
}

TEST(Dataflow, FreeOnEveryPathIsNotDoubleFree) {
  const auto result = analyze(R"c(
    void f(int c) {
      char *p = malloc(4);
      if (c) free(p); else free(p);
    }
    void g(int c) {
      char *p = malloc(4);
      if (c) { free(p); return; }
      use(p);
      free(p);
    }
    void h(void) {
      char *p = malloc(4);
      if (!p) return;
      p[0] = 1;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(Dataflow, DiagnosticsAreReportedOnceAndInSourceOrder) {
  // The loop body is visited several times before the fixpoint; each site
  // must still be reported exactly once.
  const auto result = analyze(R"c(
    void f(int n) {
      char *p = malloc(4);
      while (n--) {
        free(p);
        use(p);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"5: 'p' is freed twice", "6: use of 'p' after it was freed"}));
}

// -- Aliases ------------------------------------------------------------------

TEST(Dataflow, FreeThroughAliasNamesTheAlias) {
  const auto result = analyze(R"c(
    void f(void) {
      char *p = malloc(8);
      char *q = p;
      free(q);
      p[0] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: use of 'p' after it was freed"}));
  EXPECT_EQ(notes(result.diagnostics), (Strings{"freed here (through 'q')"}));
}

TEST(Dataflow, ConditionalExpressionAliasesBothArms) {
  const auto result = analyze(R"c(
    void f(int c) {
      char *p = malloc(4);
      char *q = malloc(4);
      char *r = c ? p : q;
      free(r);
      use(p);
      free(q);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"7: use of 'p' after it was freed", "8: 'q' is freed twice"}));
  EXPECT_EQ(notes(result.diagnostics, 1),
            (Strings{"previously freed here (through 'r')"}));
}

TEST(Dataflow, AliasOfStructPointerMirrorsFields) {
  const auto result = analyze(R"c(
    struct node { struct node *next; int *data; };
    void f(struct node *n) {
      struct node *m = n;
      free(m->data);
      use(n->data);
    }
    void g(struct node *p) {
      free(p->data);
      struct node *q = p;      /* copy after the free: facts are mirrored */
      use(q->data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: use of 'n->data' after it was freed",
                     "11: use of 'q->data' after it was freed"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"freed here (through 'm->data')"}));
  EXPECT_EQ(notes(result.diagnostics, 1),
            (Strings{"freed here (through 'p->data')"}));
}

TEST(Dataflow, FieldCopiedIntoLocalAliasesTheField) {
  const auto result = analyze(R"c(
    struct ctx { char *buf; int n; };
    void f(struct ctx *c) {
      char *b = c->buf;
      free(b);
      free(c->buf);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: 'c->buf' is freed twice"}));
}

TEST(Dataflow, FreeingAnObjectKillsItsAliases) {
  const auto result = analyze(R"c(
    struct ctx { char *buf; int n; };
    void f(void) {
      struct ctx *c = malloc(sizeof *c);
      c->buf = malloc(4);
      struct ctx *d = c;
      free(d->buf);
      free(c);
      use(d);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"9: use of 'd' after it was freed"}));
}

TEST(Dataflow, WritingAFieldRefillsItUnderEveryName) {
  // `L->twups ~ L`: `L->stack` and `L->twups->stack` are one cell. Freeing
  // it under one name and storing a fresh block under the other leaves a
  // live block, whichever name frees it next (RFC 0002, aliases).
  const auto result = analyze(R"c(
    struct th { struct th *twups; char *stack; };
    void direct(struct th *L) {
      L->twups = L;
      free(L->stack);
      L->stack = malloc(8);
      free(L->twups->stack);
    }
    void through_other(struct th *L, struct th *M) {
      M->twups = L;
      free(M->twups->stack);
      L->stack = malloc(8);
      free(M->twups->stack);
    }
    static int grow(struct th *L, int n) {
      char *ns = realloc(L->stack, n);
      if (ns == NULL) return 0;
      L->stack = ns;
      return 1;
    }
    void twice(struct th *L) {
      L->twups = L;
      grow(L, 16);
      grow(L, 32);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(Dataflow, ReassigningAnAliasSeparatesIt) {
  const auto result = analyze(R"c(
    void f(void) {
      char *p = malloc(4);
      char *q = p;
      q = malloc(4);
      free(q);
      use(p);      /* fine: q no longer aliases p */
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

// -- Linked-list idioms that must stay clean (AliasRelation is not transitive)

TEST(Dataflow, ListFreeIdiomsAreClean) {
  const auto result = analyze(R"c(
    struct node { struct node *next; int v; };
    void free_list(struct node *head) {
      while (head) {
        struct node *next = head->next;
        free(head);
        head = next;
      }
    }
    void free_list_hoisted(struct node *cur) {
      struct node *next;
      while (cur) {
        next = cur->next;
        free(cur);
        cur = next;
      }
    }
    void free_two(struct node *cur) {
      struct node *next = cur->next;
      free(cur);
      cur = next;
      free(cur);
    }
    void advance(struct node *p) {
      struct node *tmp = p;
      p = p->next;
      free(tmp);
      use(p);
    }
    void link(struct node *head) {
      struct node *prev = head;
      struct node *n = malloc(sizeof *n);
      prev->next = n;
      free(n);
      use(prev);
    }
    void unlink_nth(struct node *head, int n) {
      struct node *cur = head;
      struct node *prev = 0;
      while (cur) {
        if (n-- > 0) {
          prev = cur;
          cur = cur->next;
          continue;
        }
        struct node *victim = cur;
        cur = cur->next;
        if (prev) prev->next = cur;
        free(victim);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

// -- Structured places --------------------------------------------------------

TEST(Dataflow, StructMemberOfLocal) {
  const auto result = analyze(R"c(
    struct ctx { char *buf; int n; };
    void f(void) {
      struct ctx c;
      c.buf = malloc(4);
      free(c.buf);
      use(c.buf);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: use of 'c.buf' after it was freed"}));
}

TEST(Dataflow, NestedArrowPaths) {
  const auto result = analyze(R"c(
    struct inner { int *buf; };
    struct outer { struct inner *in; };
    void f(struct outer *p) {
      free(p->in->buf);
      use(p->in->buf);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: use of 'p->in->buf' after it was freed"}));
}

TEST(Dataflow, DerefOfParameter) {
  const auto result = analyze(R"c(
    void f(char **pp) {
      free(*pp);
      use(*pp);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: use of '*pp' after it was freed"}));
}

TEST(Dataflow, FreedObjectCannotBeDereferenced) {
  const auto result = analyze(R"c(
    struct ctx { char *buf; int n; };
    int f(struct ctx *c) {
      free(c);
      return c->n;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: use of 'c' after it was freed"}));
}

TEST(Dataflow, ArrayElementsAreOnePlaceWithWitnesses) {
  // `arr[*]` is one place (RFC 0002); the move record remembers which
  // element was named and only a matching access is a use (RFC 0006,
  // *Element witnesses*).
  const auto result = analyze(R"c(
    void constants(void) {
      int *arr[4];
      arr[0] = malloc(4);
      arr[1] = malloc(4);
      free(arr[0]);
      use(arr[1]);      /* different constant: clean */
      use(arr[0]);      /* same constant: use-after-free */
    }
    void variables(int **a, int i, int j) {
      free(a[i]);
      use(a[j]);        /* different variable: clean */
      use(a[i]);        /* same variable: use-after-free */
    }
    void whole(int **a, int i) {
      free(a[i]);
      use(*a);          /* no subscript: matches any element */
      free(a);          /* the array itself is another place: clean */
    }
    void first(void) {
      int *arr[4];
      arr[0] = malloc(4);
      free(arr[0]);
      use(*arr);        /* `*arr` on an array is `arr[0]` */
    }
    void repeated(int **a) {
      free(a[0]);
      free(a[0]);       /* double-free */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: use of 'arr[*]' after it was freed",
                     "13: use of '*a' after it was freed",
                     "17: use of '*a' after it was freed",
                     "24: use of 'arr[*]' after it was freed",
                     "28: '*a' is freed twice"}));
}

TEST(Dataflow, ElementWitnessesGoStaleWithTheirVariable) {
  // A write to, increment of, or address-of the index variable turns the
  // witness unknown: it then matches nothing but a whole access (RFC 0006,
  // *Element witnesses*).
  const auto result = analyze(R"c(
    void loop_free(char **a, int n) {
      for (int i = 0; i < n; i++) free(a[i]);
      free(a);
    }
    void null_out(char **a, int n) {
      for (int i = 0; i < n; i++) { free(a[i]); a[i] = NULL; }
      use(a[0]);
    }
    void incremented(char **a, int i) {
      free(a[i]);
      i++;
      use(a[i]);        /* another element now: clean */
    }
    void reassigned(char **a, int i, int j) {
      free(a[i]);
      i = j;
      use(a[i]);        /* clean */
    }
    void unknown_index(char **a, int i) {
      free(a[i + 1]);
      use(a[i + 1]);    /* not a recognised witness: clean */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(Dataflow, ElementWitnessesSurviveJoinsOnlyWhenTheyAgree) {
  const auto result = analyze(R"c(
    void agree(char **a, int i) {
      if (cond()) free(a[i]); else free(a[i]);
      use(a[i]);        /* use-after-free */
    }
    void disagree(char **a, int i, int j) {
      if (cond()) free(a[i]); else free(a[j]);
      use(a[i]);        /* witness unknown: clean */
      use(a[0]);        /* clean */
    }
    void one_side_whole(char **a, int i) {
      if (cond()) free(a[i]); else free(*a);
      use(a[i]);        /* whole on one side matches: reported */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: use of '*a' after it was freed",
                     "13: use of '*a' after it was freed"}));
}

// -- Moves --------------------------------------------------------------------

TEST(Dataflow, OwnedParameterIsMovedIntoCallee) {
  const auto result = analyze(R"c(
    void f(int *OWNED p) {
      take(p);
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{std::string(core::diag::UseAfterMove)}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: use of 'p' after it was moved"}));
  EXPECT_EQ(notes(result.diagnostics), (Strings{"moved here"}));
}

TEST(Dataflow, NullAssignmentReinitialises) {
  const auto result = analyze(R"c(
    void f(void) {
      char *p = malloc(4);
      free(p);
      p = NULL;
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

// -- realloc ------------------------------------------------------------------

TEST(Dataflow, ReallocFailurePathKeepsOldPointerAlive) {
  const auto result = analyze(R"c(
    int f(char **buf) {
      char *p = *buf;
      char *q = realloc(p, 16);
      if (q == NULL) {
        free(p);
        return -1;
      }
      *buf = q;
      return 0;
    }
    void g(void) {
      char *p = malloc(4);
      p = realloc(p, 8);
      if (!p) return;
      free(p);
    }
    void h(char *p) {
      char *q = realloc(p, 8);
      use(q);
      if (q == NULL) free(p);
    }
    void i(char *p, int c) {
      char *q = realloc(p, 8);
      if (c) use(q);
      if (q == NULL) free(p);   /* the pending entry survives the join */
    }
    void j(char *p, int n) {
      for (int k = 0; k < n; ++k) {
        char *q = realloc(p, 8);
        if (!q) { free(p); return; }
        p = q;
      }
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // No use-after-move anywhere; `h` and `i` drop the grown block when
  // `realloc` succeeds, which RFC 0007 reports where `q` dies.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"21: 'q' is leaked", "26: 'q' is leaked"}));
}

TEST(Dataflow, ReallocArgumentIsMovedWithoutNullTest) {
  const auto result = analyze(R"c(
    int f(char *p) {
      char *q = realloc(p, 16);
      free(p);
      use(q);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{std::string(core::diag::UseAfterMove),
                     std::string(core::diag::Leak)}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: use of 'p' after it was moved", "6: 'q' is leaked"}));
}

TEST(Dataflow, ReallocPendingEntryDiesWithItsResult) {
  const auto result = analyze(R"c(
    void f(char *p) {
      char *q = realloc(p, 8);
      q = malloc(2);
      if (q == NULL) free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"4: 'q' is leaked: it is overwritten without being released",
               "5: 'q' is leaked", "5: use of 'p' after it was moved"}));
}

TEST(Dataflow, ReallocIntoAliasSeparatesIt) {
  const auto result = analyze(R"c(
    void f(void) {
      char *p = malloc(4);
      char *q = p;
      q = realloc(p, 8);
      if (q != NULL) { free(q); return; }
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

// -- Borrows ------------------------------------------------------------------
//
// Exclusivity between borrows (two mutable, shared then mutable, a write
// while borrowed) is RFC 0001's rule and is opt-in under
// `AnalysisOptions::exclusiveBorrows` (RFC 0006, *Conflict rules*); the
// default only rejects freeing or moving a borrowed object.

const analysis::AnalysisOptions Exclusive{.exclusiveBorrows = true};

TEST(Dataflow, TwoMutableBorrowsConflict) {
  const std::string code = R"c(
    void f(void) {
      int x = 0;
      int *a = &x;
      int *b = &x;
      use(a); use(b);
    }
  )c";
  const auto lenient = analyze(code);
  ASSERT_TRUE(lenient.ast);
  EXPECT_TRUE(lenient.diagnostics.empty());

  const auto result = analyze(code, Exclusive);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{std::string(core::diag::ConflictingBorrow)}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot borrow 'x' as mutable because it is already "
                     "borrowed"}));
  EXPECT_EQ(notes(result.diagnostics),
            (Strings{"previous borrow of 'x' by 'a' here"}));
}

TEST(Dataflow, SharedBorrowsCoexist) {
  const auto result = analyze(R"c(
    void f(void) {
      int x = 0;
      const int *a = &x;
      const int *b = &x;
      use((void *)a); use((void *)b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
}

TEST(Dataflow, SharedThenMutableConflicts) {
  const std::string code = R"c(
    void f(void) {
      int x = 0;
      const int *a = &x;
      int *b = &x;
      use((void *)a); use(b);
    }
  )c";
  EXPECT_TRUE(analyze(code).diagnostics.empty());
  const auto result = analyze(code, Exclusive);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot borrow 'x' as mutable because it is already "
                     "borrowed"}));
}

TEST(Dataflow, MutableThenSharedConflicts) {
  const std::string code = R"c(
    void f(void) {
      int x = 0;
      int *a = &x;
      const int *b = &x;
      use(a); use((void *)b);
    }
  )c";
  EXPECT_TRUE(analyze(code).diagnostics.empty());
  const auto result = analyze(code, Exclusive);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot borrow 'x' as shared because it is already "
                     "mutably borrowed"}));
}

TEST(Dataflow, WritingABorrowedObjectConflicts) {
  const std::string code = R"c(
    void f(void) {
      int x = 0;
      int *a = &x;
      x = 1;
      use(a);
    }
  )c";
  EXPECT_TRUE(analyze(code).diagnostics.empty());
  const auto result = analyze(code, Exclusive);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot assign to 'x' while it is borrowed"}));
  EXPECT_EQ(notes(result.diagnostics), (Strings{"borrowed by 'a' here"}));
}

TEST(Dataflow, MutationWhileViewedIsTheDefaultIdiom) {
  // RFC 0006, snippets that must be clean: a pointer that views a buffer
  // another routine writes is how string handling in C works.
  const auto result = analyze(R"c(
    struct s { int a; int b; };
    void two_views(struct s *s) { int *pa = &s->a; *pa = 1; s->a = 2; use(pa); }
    void buffer(void) { char buf[8]; char *p = buf; poke(buf); use(p); }
    void two_mutable(void) { int x; int *a = &x; int *b = &x; *a = 1; *b = 2; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(Dataflow, FreeingABorrowedObjectConflicts) {
  const auto result = analyze(R"c(
    struct node { int v; };
    void f(void) {
      struct node *n = malloc(sizeof *n);
      int *a = &n->v;
      free(n);
      use(a);
    }
    void g(struct node *p) {
      int *a = &p->v;
      struct node *r = p;
      free(r);
      use(a);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: cannot free 'n' while it is borrowed",
                     "12: cannot free 'r' while it is borrowed"}));
}

TEST(Dataflow, FreeingBelowABorrowedObjectIsNotAConflict) {
  // The loan is on the struct; what is freed is what one of its fields
  // points to, storage the loan never covered (RFC 0006, *Conflict rules*).
  const auto result = analyze(R"c(
    struct stream { char *window; int n; };
    struct state { struct stream strm; int size; };
    static void reset(struct stream *s) { free(s->window); s->window = 0; }
    int f(struct state *st) {
      struct stream *strm = &st->strm;
      reset(&st->strm);
      strm->n = 0;
      free(st->strm.window);
      return strm->n;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(Dataflow, MovingABorrowedObjectConflicts) {
  const auto result = analyze(R"c(
    struct node { int v; };
    void f(struct node *OWNED n) {
      int *a = &n->v;
      take(n);
      use(a);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot move 'n' while it is borrowed"}));
}

TEST(Dataflow, BorrowsEndWhenTheHolderDiesOrIsReassigned) {
  const auto result = analyze(R"c(
    void through(void) {
      int x = 0;
      int *a = &x;
      *a = 5;          /* writes through the borrow are fine */
      use(a);
    }
    void released(void) {
      int x = 0;
      int *a = &x;
      a = NULL;
      x = 1;
      use(a);
    }
    void scoped(void) {
      int x = 0;
      {
        int *a = &x;
        use(a);
      }
      x = 1;
    }
    void looped(int n) {
      int x = 0;
      for (int i = 0; i < n; ++i) {
        int *a = &x;
        use(a);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

TEST(Dataflow, TemporaryBorrowsForAnnotatedArguments) {
  const std::string code = R"c(
    void f(void) {
      int x = 0;
      peek(&x);
      poke(&x);        /* fine: the temporary borrows ended */
      int *a = &x;
      poke(&x);
      use(a);
    }
  )c";
  EXPECT_TRUE(analyze(code).diagnostics.empty());
  const auto result = analyze(code, Exclusive);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: cannot borrow 'x' as mutable because it is already "
                     "borrowed"}));
}

TEST(Dataflow, ArrayDecayBorrowsTheElements) {
  const std::string code = R"c(
    void f(void) {
      int a[4];
      int *p = a;
      int *q = &a[1];
      use(p); use(q);
    }
  )c";
  EXPECT_TRUE(analyze(code).diagnostics.empty());
  const auto result = analyze(code, Exclusive);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot borrow 'a[*]' as mutable because it is already "
                     "borrowed"}));
}

TEST(Dataflow, LoansEndAtTheLastUseOfTheHolder) {
  // RFC 0006, *Loans end at the last use of their holder*: after `use(p)`
  // the loan is gone even though `p` is still in scope, so the object may
  // be freed, moved, or (under exclusivity) borrowed again.
  const std::string code = R"c(
    struct node { int v; };
    void last_use(void) { char buf[8]; char *p = buf; use(p); buf[0] = 0; }
    void free_after(struct node *OWNED n) {
      int *a = &n->v;
      *a = 1;
      free(n);                    /* `a` is dead: fine */
    }
    void reborrow(void) {
      int x = 0;
      int *a = &x;
      use(a);
      int *b = &x;                /* `a` is dead: fine even when exclusive */
      use(b);
    }
    void still_live(struct node *OWNED n) {
      int *a = &n->v;
      free(n);                    /* conflict: `a` is used below */
      *a = 1;
    }
    void through_pointer(struct node *OWNED n, int **out) {
      *out = &n->v;
      free(n);                    /* the holder is not a local: conflict */
    }
    void address_taken(struct node *OWNED n) {
      int *a = &n->v;
      int **pa = &a;
      free(n);                    /* `a` may be read through `pa`: conflict */
      use(pa);
    }
    void in_loop(struct node *OWNED n, int k) {
      int *a = &n->v;
      for (int i = 0; i < k; i++) {
        if (i == 5) { free(n); break; }   /* conflict: `a` is used below */
      }
      *a = 1;
    }
    void loop_done(struct node *OWNED n, int k) {
      int *a = &n->v;
      for (int i = 0; i < k; i++) *a += i;
      free(n);                    /* `a` is dead: fine */
    }
  )c";
  for (const analysis::AnalysisOptions &options :
       {analysis::AnalysisOptions{}, Exclusive}) {
    const auto result = analyze(code, options);
    ASSERT_TRUE(result.ast);
    // `in_loop` never frees `n` when the loop runs to completion (RFC 0007).
    EXPECT_EQ(messages(result.diagnostics),
              (Strings{"18: cannot free 'n' while it is borrowed",
                       "23: cannot free 'n' while it is borrowed",
                       "28: cannot free 'n' while it is borrowed",
                       "34: cannot free 'n' while it is borrowed",
                       "36: 'n' is leaked"}))
        << "exclusive=" << options.exclusiveBorrows;
  }
}

TEST(Dataflow, DeadLocalsDropTheirAliasEdgesWithoutLosingFacts) {
  // RFC 0006, *Performance*: a dead local leaves the alias relation. Every
  // fact it carried was propagated to its aliases when it was made, so
  // nothing observable changes; only the dead name is gone.
  const auto result = analyze(R"c(
    void freed_through_dead_alias(char *OWNED p) {
      char *q = p;
      free(q);                    /* q is dead from here */
      use(p);                     /* p carries the record itself: reported */
    }
    void dead_alias_is_not_revived(char *OWNED p) {
      char *q = p;
      use(q);                     /* q is dead from here */
      free(p);
      q = malloc(4);
      use(q);                     /* a new value: clean */
      free(q);
    }
    void both_live(char *OWNED p) {
      char *q = p;
      free(p);
      use(q);                     /* reported */
    }
    int through_dead_param_alias(struct n { int v; } *n) {
      struct n *m = n;            /* n is dead from here, but a parameter */
      return m->v;                /* still a borrow of n in the summary */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: use of 'p' after it was freed",
                     "18: use of 'q' after it was freed"}));
  const core::FunctionSummary *viaParam =
      result.summary("through_dead_param_alias");
  ASSERT_NE(viaParam, nullptr);
  EXPECT_EQ(viaParam->borrowKind(0), core::BorrowKind::Shared);
}

// -- Lifetimes ----------------------------------------------------------------

TEST(Dataflow, ReturningAddressOfLocal) {
  const auto result = analyze(R"c(
    int *f(void) {
      int x = 1;
      int *p = &x;
      return p;
    }
    int *g(void) {
      int x = 1;
      return &x;   /* also -Wreturn-stack-address; ours must agree */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{std::string(core::diag::LifetimeTooShort),
                     std::string(core::diag::LifetimeTooShort)}));
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"5: 'p' may outlive 'x', which it points to",
               "9: returned pointer may outlive 'x', which it points to"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"'x' is declared here"}));
}

TEST(Dataflow, PointerOutlivesInnerScope) {
  const auto result = analyze(R"c(
    void f(void) {
      int *p;
      {
        int x = 1;
        p = &x;
      }
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: 'p' may outlive 'x', which it points to"}));
  ASSERT_EQ(notes(result.diagnostics),
            (Strings{"'x' is declared here", "'x' goes out of scope here"}));
  EXPECT_EQ(result.diagnostics.diagnostics()[0].notes[1].location.line, 7U);
}

TEST(Dataflow, EscapeThroughOutParameterOrGlobal) {
  const auto result = analyze(R"c(
    int *gp;
    void f(int **out) {
      int x = 1;
      *out = &x;
    }
    void g(void) {
      int x = 1;
      gp = &x;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: '*out' may outlive 'x', which it points to",
                     "9: 'gp' may outlive 'x', which it points to"}));
}

TEST(Dataflow, LifetimesThatDoOutlive) {
  const auto result = analyze(R"c(
    static int g;
    int *gp;
    void ok(void) {
      int x = 1;
      {
        int *p = &x;
        use(p);
      }
    }
    void assigned_inside(void) {
      int *p;
      int x = 0;
      {
        p = &x;
      }
      use(p);
    }
    void statics(void) {
      static int s;
      gp = &s;
      int *p = &g;
      use(p);
    }
    void jump_out(int c) {
      char *p = malloc(4);
      {
        int y = 1;
        if (c) goto done;
        use(&y);
      }
    done:
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

// -- Pointer identity (RFC 0004) ----------------------------------------------

TEST(Dataflow, PointerArithmeticPreservesIdentity) {
  // `p + 1` and `p++` denote the same object as `p` (RFC 0004, *Pointer
  // identity*), so a free through one is a free of the other.
  const auto result = analyze(R"c(
    void f(void) {
      char *p = malloc(4);
      char *q = p + 1;
      free(p);
      use(q);
    }
    void g(void) {
      char *p = malloc(4);
      p++;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            Strings{"6: use of 'q' after it was freed"});
  EXPECT_EQ(notes(result.diagnostics, 0), Strings{"freed here (through 'p')"});
}

TEST(Dataflow, SelfAssignmentKeepsEveryFact) {
  // `cur = cur + 1` means the same as `cur++`: the place keeps its aliases,
  // and so do `cur = cur` and `cur = (T *)cur` (RFC 0004, *Pointer
  // identity*).
  const auto result = analyze(R"c(
    struct node { int v; };
    int a(struct node *OWNED head) {
      struct node *cur = head;
      cur = cur + 1;
      free(cur);
      return head->v;
    }
    int b(struct node *OWNED head) {
      struct node *cur = head;
      cur = cur;
      free(cur);
      return head->v;
    }
    int c(struct node *OWNED head) {
      struct node *cur = head;
      cur = (struct node *)(void *)cur;
      free(cur);
      return head->v;
    }
    void d(long x) {
      int *r = (int *)x;
      r = r + 1;
      *r = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"7: use of 'head' after it was freed",
               "13: use of 'head' after it was freed",
               "19: use of 'head' after it was freed",
               "24: dereference of raw pointer 'r' outside an unsafe region"}));
  EXPECT_EQ(notes(result.diagnostics, 3)[0],
            "'r' is raw: cast from an integer here");
}

TEST(Dataflow, PointerCastsPreserveIdentity) {
  // `(struct sockaddr *)&addr` is still `addr` (RFC 0004, *Pointer
  // identity*): a pointer-to-pointer cast changes the type, not the object.
  const auto result = analyze(R"c(
    struct a { int x; };
    struct b { int y; };
    void f(void) {
      struct a *p = malloc(sizeof *p);
      struct b *q = (struct b *)p;
      free(q);
      use((char *)p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            Strings{"8: use of 'p' after it was freed"});
}

TEST(Dataflow, IntegerCastsYieldRawPointers) {
  // Only a round trip through an integer loses provenance; the result is a
  // raw pointer, and dereferencing it is a raw operation (RFC 0004, *Raw
  // pointers*).
  const auto result = analyze(R"c(
    void f(long x) {
      int *p = (int *)x;
      *p = 1;
    }
    void g(long x) {
      int *p = (int *)x;
      int *q = p;
      use((char *)q);
      if (q == 0) return;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"4: dereference of raw pointer 'p' outside an unsafe region",
               "9: 'use' dereferences raw pointer 'q' outside an unsafe "
               "region"}))
      << "copies and comparisons of a raw pointer are fine";
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"unsafe-operation", "unsafe-operation"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"'p' is raw: cast from an integer here",
                     "move this operation into a WEAVEC_UNSAFE block or "
                     "function, or assert the pointer's ownership first"}));
  EXPECT_EQ(notes(result.diagnostics, 1)[0],
            "'q' is raw: cast from an integer here (through 'p')");
}

// -- Raw pointers and unsafe regions (RFC 0004)
// --------------------------------

TEST(Dataflow, RawOperationsAreReleaseAndOwnershipTransferToo) {
  const auto result = analyze(R"c(
    void f(long x) {
      char *p = (char *)x;
      free(p);
    }
    void g(long x) {
      char *p = (char *)x;
      take(p);
    }
    void h(long x) {
      char *p = (char *)x;
      poke(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"4: 'free' releases raw pointer 'p' outside an unsafe region",
               "8: 'take' takes ownership of raw pointer 'p' outside an "
               "unsafe region",
               "12: 'poke' dereferences raw pointer 'p' outside an unsafe "
               "region"}));
}

TEST(Dataflow, RawAnnotationOnParametersFieldsAndLocals) {
  const auto result = analyze(R"c(
    struct ctx { void *RAW cookie; int *plain; };
    void param(int *RAW r) { *r = 1; }
    void field(struct ctx *c) {
      int *p = c->cookie;
      *p = 1;
      *c->plain = 1;        /* an ordinary field */
    }
    void local(int *q) {
      int *RAW r = q;       /* q stays tracked; r is raw */
      *r = 1;
      *q = 1;
    }
    void raw_stays_raw_when_reassigned(int *RAW r, int *q) {
      r = q;                /* the place is declared raw: still raw */
      *r = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"3: dereference of raw pointer 'r' outside an unsafe region",
               "6: dereference of raw pointer 'p' outside an unsafe region",
               "11: dereference of raw pointer 'r' outside an unsafe region",
               "16: dereference of raw pointer 'r' outside an unsafe region"}));
  EXPECT_EQ(notes(result.diagnostics, 0)[0],
            "'r' is raw: declared WEAVEC_RAW here");
  EXPECT_EQ(notes(result.diagnostics, 1)[0],
            "'p' is raw: declared WEAVEC_RAW here (through 'c->cookie')");
}

TEST(Dataflow, UnsafeRegionPermitsRawOperationsAndSuppressesReports) {
  const auto result = analyze(R"c(
    void f(long x, int *p) {
      UNSAFE {
        int *r = (int *)x;
        *r = 1;
        free(r);
        free(p);
        free(p);
      }
    }
    UNSAFE void g(int *RAW r) { *r = 1; free(r); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
  // The unsafe function still has a summary its callers use.
  ASSERT_NE(result.summary("g"), nullptr);
  EXPECT_TRUE(result.summary("g")->frees(0));
}

TEST(Dataflow, LaunderingByAssertion) {
  // RFC 0004, "Laundering": storing a raw value into a place declared with a
  // safe kind, or returning it from a function whose return type is
  // annotated, asserts that kind. Fine inside an unsafe region, an error
  // outside; the kind holds afterwards either way.
  const auto result = analyze(R"c(
    struct box { int *OWNED owned; };
    int *OWNED by_return(long x) {
      UNSAFE { return (int *)x; }
    }
    int *OWNED by_return_outside(long x) {
      return (int *)x;
    }
    void by_local(long x) {
      OWNED int *p;
      UNSAFE { p = (int *)x; }
      free(p);
      free(p);
    }
    void by_local_outside(long x) {
      int *raw = (int *)x;
      OWNED int *p = raw;
      free(p);
    }
    void by_field(struct box *b, long x) {
      UNSAFE { b->owned = (int *)x; }
      free(b->owned);
      use(b->owned);
    }
    void caller(long x) {
      int *p = by_return(x);
      *p = 1;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"7: raw pointer is returned from a function whose return type "
               "is annotated WEAVEC_OWNED outside an unsafe region",
               "13: 'p' is freed twice",
               "17: raw pointer 'raw' is assigned to 'p', which is declared "
               "WEAVEC_OWNED, outside an unsafe region",
               "23: use of 'b->owned' after it was freed"}));
  // What the body hands back is raw, but the annotation is the contract:
  // callers see an owned result (and `caller` above therefore has nothing to
  // report).
  EXPECT_EQ(result.summary("by_return")->inferredReturnKind(),
            core::OwnershipKind::Raw);
  const auto resolved =
      result.analyzer->summaries().lookup(*result.function("by_return"));
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->source, analysis::SummarySource::Annotation);
  EXPECT_EQ(resolved->summary->inferredReturnKind(),
            core::OwnershipKind::Owned);
}

TEST(Dataflow, RawIsAValueSourceInSummaries) {
  const auto result = analyze(R"c(
    struct s { int *field; };
    static int *from_int(long x) { return (int *)x; }
    static void store_raw(struct s *s, long x) { s->field = (int *)x; }
    static int *RAW declared(void *RAW p) { return p; }
    void caller(struct s *s, long x) {
      int *a = from_int(x);
      *a = 1;
      store_raw(s, x);
      *s->field = 2;
      int *b = declared(a);
      *b = 3;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(result.summary("from_int")->returns,
            std::set<core::ValueSource>{core::ValueSource::raw()});
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"8: dereference of raw pointer 'a' outside an unsafe region",
               "10: dereference of raw pointer 's->field' outside an unsafe "
               "region",
               "12: dereference of raw pointer 'b' outside an unsafe region"}));
  EXPECT_EQ(notes(result.diagnostics, 0)[0],
            "'a' is raw: handed out by 'from_int' here");
  EXPECT_EQ(notes(result.diagnostics, 1)[0],
            "'s->field' is raw: handed out by 'store_raw' here");
}

// -- Indirect calls (RFC 0004, "Signatures for function pointers")
// -------------

TEST(Dataflow, IndirectCallsUseTypeAnnotationsOrAddressTakenJoin) {
  const auto result = analyze(R"c(
    struct node { int v; };
    typedef void (*dtor_t)(struct node *OWNED);
    typedef OWNED struct node *(*maker_t)(void);
    static void node_free(struct node *n) { free(n); }
    struct hooks { void (*drop)(struct node *); };
    static struct hooks H = { node_free };
    void a(dtor_t d, struct node *n) { d(n); use(n); }
    void b(maker_t m) { struct node *n = m(); free(n); use(n); }
    void c(struct node *n) { H.drop(n); use(n); }
    void d(void (*cb)(struct node *), struct node *n) { cb(n); use(n); }
    void e(int (*cmp)(int, int), struct node *n) { cmp(1, 2); use(n); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: use of 'n' after it was moved",
                     "9: use of 'n' after it was freed",
                     "10: use of 'n' after it was freed",
                     "11: use of 'n' after it was freed"}))
      << "`cmp` has no pointer parameters: not even a boundary warning";
}

TEST(Dataflow, CallbacksAreAnalysedBeforeTheirCallers) {
  // The call graph has an edge from an indirect call to every address-taken
  // function of the type, so `node_free` is summarised before `f`, whichever
  // comes first in the file.
  const auto result = analyze(R"c(
    struct node { int v; };
    static void node_free(struct node *n);
    void f(void (*cb)(struct node *), struct node *n) { cb(n); use(n); }
    static void (*registered)(struct node *) = node_free;
    static void node_free(struct node *n) { free(n); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            Strings{"4: use of 'n' after it was freed"});
}

TEST(Dataflow, StructCopiesCopyTheirPointerFields) {
  // RFC 0005, *Struct copies*: `b = a` is `b.f = a.f` for every pointer
  // field, so `b.data` aliases `a.data` and shares its move records.
  const auto result = analyze(R"c(
    struct buf { char *data; int n; };
    struct outer { struct buf b; char *tag; };
    int init_copy(void) {
      struct buf a = { malloc(8), 8 };
      struct buf b = a;
      free(a.data);
      return b.data[0];
    }
    int assign_copy(void) {
      struct buf a; a.data = malloc(8);
      struct buf b; b = a;
      free(b.data);
      free(a.data);
      return 0;
    }
    int nested(void) {
      struct outer o = { { malloc(4), 4 }, malloc(2) };
      struct outer p = o;
      free(o.b.data);
      return p.b.data[0];
    }
    int literal(void) {
      struct buf a;
      a = (struct buf){ .n = 8, .data = malloc(8) };
      free(a.data);
      return a.data[0];
    }
    int through_pointer(struct buf *p) {
      struct buf local = *p;
      free(local.data);
      return p->data[0];
    }
    int fine(void) {
      struct buf a = { malloc(8), 8 };
      struct buf b = a;
      b.data[0] = 1;
      free(a.data);
      return 0;
    }
    struct buf make(void);
    int opaque(void) {
      struct buf a = make();
      free(a.data);
      return a.n;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: use of 'b.data' after it was freed",
                     "14: 'a.data' is freed twice", "21: 'p.tag' is leaked",
                     "21: use of 'p.b.data' after it was freed",
                     "27: use of 'a.data' after it was freed",
                     "32: use of 'p->data' after it was freed"}));
}

// -- Condition facts (RFC 0006) -----------------------------------------------

TEST(Dataflow, PointerEqualityRefinesAliasesOnEdges) {
  const auto result = analyze(R"c(
    struct list { struct list *next; };
    void unequal(char *p, char *q) {
      if (p != q) { free(p); use(q); }          /* distinct: clean */
    }
    void equal(char *p, char *q) {
      if (p == q) { free(p); use(q); }          /* same object: reported */
    }
    void inverted(char *p, char *q) {
      if (p == q) return;
      free(p); use(q);                          /* distinct: clean */
    }
    void interior(char *p) {
      char *q = p + 1;
      if (p != q) { free(p); use(q); }          /* interior alias: reported */
    }
    void exact_copy(char *p) {
      char *r = p;
      if (r != p) { free(p); use(r); }          /* infeasible, but the model
                                                   only separates: clean */
    }
    void rejoined(char *p, char *q) {
      char *r = p;
      if (r != q) use(q);
      free(p); use(r);                          /* joined back: reported */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: use of 'q' after it was freed",
                     "15: use of 'q' after it was freed",
                     "25: use of 'r' after it was freed"}));
}

TEST(Dataflow, AssignmentInsideAPointerTestNamesItsLeftSide) {
  // The linenoise idiom: read until the reader stops returning the
  // sentinel. `(res = feed()) == sentinel` is a fact about `res`; on the exit
  // edge it is not the sentinel, so freeing it frees only the fresh line.
  const auto result = analyze(R"c(
    char sentinel_storage[1];
    char *sentinel = sentinel_storage;
    static char *feed(int c) { return c ? malloc(8) : sentinel; }
    void loop(int c) {
      char *res;
      while ((res = feed(c)) == sentinel)
        ;
      free(res);
      use(sentinel);                          /* clean */
    }
    void flipped(int c) {
      char *res;
      while (sentinel == (res = feed(c)))
        ;
      free(res);
      use(sentinel);                          /* clean */
    }
    void taken(int c) {
      char *res;
      if ((res = feed(c)) == sentinel) {
        free(res);
        use(sentinel);                        /* the sentinel itself: reported */
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  // `taken` keeps the fresh line when the test fails (RFC 0007).
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"21: 'res' is leaked",
                     "23: use of 'sentinel' after it was freed"}));
  const core::FunctionSummary *feed = result.summary("feed");
  ASSERT_NE(feed, nullptr);
  EXPECT_EQ(feed->returns.size(), 2U) << "fresh or a copy of the global";
}

TEST(Dataflow, OutcomeTestsSelectClasses) {
  // Every recognised shape of test on a call result (RFC 0006, *Outcome
  // tests*), on a callee that consumes only when it returns 0.
  const auto result = analyze(R"c(
    static int try_take(char *p, int c) { if (c) { free(p); return 0; } return -1; }
    static int try_pos(char *p, int c) { if (c) { free(p); return 1; } return 0; }
    static char *try_ptr(char *p, int c) { if (c) { free(p); return NULL; } return p; }
    void bang(char *p, int c) { if (!try_take(p, c)) return; free(p); }
    void eq0(char *p, int c) { int r = try_take(p, c); if (r == 0) return; free(p); }
    void ne0(char *p, int c) { int r = try_take(p, c); if (r != 0) free(p); }
    void lt0(char *p, int c) { int r = try_take(p, c); if (r < 0) free(p); }
    void ge0(char *p, int c) { if (try_take(p, c) >= 0) return; free(p); }
    void eqm1(char *p, int c) { int r = try_take(p, c); if (r == -1) free(p); }
    void truthy(char *p, int c) { if (try_pos(p, c)) return; free(p); }
    void assigned(char *p, int c) { int r; if ((r = try_take(p, c)) == 0) return; free(p); }
    void ptr_null(char *p, int c) { char *q = try_ptr(p, c); if (q == NULL) return; free(q); }
    void ptr_bang(char *p, int c) { if (!try_ptr(p, c)) return; free(p); }
    void ptr_truthy(char *p, int c) { char *q = try_ptr(p, c); if (q) free(p); }
    void wrong_side(char *p, int c) { int r = try_take(p, c); if (r == 0) free(p); }
    void ge1_misses(char *p, int c) { int r = try_take(p, c); if (r > -2) free(p); }
    void untested(char *p, int c) { try_take(p, c); free(p); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"16: 'p' is freed twice", "17: 'p' is freed twice",
                     "18: 'p' is freed twice"}));
}

TEST(Dataflow, OutcomeConditionalSummariesAreInferred) {
  const auto result = analyze(R"c(
    static int try_take(char *p, int c) { if (c) { free(p); return 0; } return -1; }
    static char *grow(char *p, size_t n) { char *q = realloc(p, n); if (!q) return NULL; return q; }
    static char *grow_direct(char *p, size_t n) { return realloc(p, n); }
    static void always(char *p, int c) { if (c) free(p); else free(p); }
    static int unconditional(char *p) { free(p); return cond(); }
    void caller(char *p) { char *q = grow(p, 8); if (!q) { free(p); return; } free(q); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];

  using core::Outcome;
  const core::SummaryPath p = core::SummaryPath::param(0);
  const core::FunctionSummary *tryTake = result.summary("try_take");
  ASSERT_NE(tryTake, nullptr);
  EXPECT_TRUE(tryTake->frees(0));
  EXPECT_FALSE(tryTake->consumesUnconditionally(p));
  EXPECT_TRUE(tryTake->outcomes.at(Outcome::Zero).at(p).freed);
  EXPECT_TRUE(tryTake->outcomes.at(Outcome::Negative).empty());
  EXPECT_FALSE(tryTake->outcomes.contains(Outcome::Positive));

  for (const char *name : {"grow", "grow_direct"}) {
    const core::FunctionSummary *grow = result.summary(name);
    ASSERT_NE(grow, nullptr) << name;
    EXPECT_TRUE(grow->consumes(0)) << name;
    EXPECT_FALSE(grow->consumesUnconditionally(p)) << name;
    EXPECT_TRUE(grow->outcomes.at(Outcome::NonNull).at(p).moved) << name;
    EXPECT_TRUE(grow->outcomes.at(Outcome::Null).empty()) << name;
  }

  // Nothing conditional: no classes are recorded at all.
  for (const char *name : {"always", "unconditional"}) {
    const core::FunctionSummary *summary = result.summary(name);
    ASSERT_NE(summary, nullptr) << name;
    EXPECT_TRUE(summary->frees(0)) << name;
    EXPECT_TRUE(summary->outcomes.empty()) << name;
    EXPECT_TRUE(summary->consumesUnconditionally(p)) << name;
  }
}

TEST(Dataflow, ACopyOfAConsumedPathIsTheResultsOwnResource) {
  // RFC 0006, *Interaction with existing RFCs*: Lua's `resizearray`. The
  // callee reallocates `t->array` and, when nothing needs doing, returns it
  // as is. The result is the resource, not a dangling copy of the old one.
  const auto result = analyze(R"c(
    struct t { char *array; size_t n; };
    static char *resize(struct t *t, size_t n) {
      if (n == t->n) return t->array;
      return realloc(t->array, n);
    }
    void grow(struct t *t, size_t n) {
      char *na = resize(t, n);
      if (na == NULL) return;             /* clean: `na` is its own value */
      t->array = na;
      use(t->array);
    }
    void forgot_to_store(struct t *t, size_t n) {
      resize(t, n);
      use(t->array);                      /* may have been reallocated: reported */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"15: use of 't->array' after it was moved"}));
  const core::FunctionSummary *resize = result.summary("resize");
  ASSERT_NE(resize, nullptr);
  const core::SummaryPath array =
      core::SummaryPath::param(0).deref().field("array");
  EXPECT_TRUE(resize->effectOf(array).moved);
  EXPECT_TRUE(resize->returns.contains(core::ValueSource::copy(array)));
}

// -- `written` forgets what lies below (RFC 0006) -----------------------------

TEST(Dataflow, WrittenObjectsForgetTheirSubobjects) {
  const auto result = analyze(R"c(
    void *memcpy(void *, const void *, size_t);
    struct n { char *string; int v; };
    struct n tmp;
    static void fill(struct n *MUT out) { out->string = malloc(4); }
    void replace(struct n *root) {
      free(root->string);
      memcpy(root, &tmp, sizeof *root);
      use(root->string);                  /* overwritten: clean */
    }
    void refilled(struct n *root) {
      free(root->string);
      fill(root);
      use(root->string);                  /* the store re-established it */
    }
    void untouched(struct n *root, struct n *other) {
      free(root->string);
      memcpy(other, &tmp, sizeof *other);
      use(root->string);                  /* another object: reported */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"19: use of 'root->string' after it was freed"}));
  const core::FunctionSummary *replace = result.summary("replace");
  ASSERT_NE(replace, nullptr);
  const core::SummaryPath stringPath =
      core::SummaryPath::param(0).deref().field("string");
  const auto it = replace->effects.find(stringPath);
  EXPECT_TRUE(it == replace->effects.end() || !it->second.freed)
      << "the exit state no longer has the freed record";
}

// A summary's written paths are looked up in order, sharing roots and
// prefixes; a `&x` argument roots its paths at `x` itself.
TEST(Dataflow, WrittenPathsForgetOnlyWhatTheyName) {
  const auto result = analyze(R"c(
    void *memcpy(void *, const void *, size_t);
    struct in { char *a; char *b; };
    struct out { struct in i; struct in j; int n; };
    struct in tmp;
    static void touch(struct out *o, struct in *k) {
      o->n = 1;
      memcpy(&o->j, &tmp, sizeof tmp);
      memcpy(k, &tmp, sizeof tmp);
    }
    void caller(struct out *o) {
      struct in local;
      local.a = malloc(1);
      free(o->i.a);
      free(o->j.a);
      free(local.a);
      touch(o, &local);
      use(o->i.a);                        /* not overwritten: reported */
      use(o->j.a);                        /* below o->j: clean */
      use(local.a);                       /* below *k, which is local */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"18: use of 'o->i.a' after it was freed"}));
  const core::FunctionSummary *touch = result.summary("touch");
  ASSERT_NE(touch, nullptr);
  EXPECT_TRUE(
      touch->effectOf(core::SummaryPath::param(0).deref().field("j")).written);
  EXPECT_TRUE(
      touch->effectOf(core::SummaryPath::param(0).deref().field("n")).written);
  EXPECT_TRUE(touch->effectOf(core::SummaryPath::param(1).deref()).written);
}

TEST(Dataflow, StructCopiesCarryLoansAndKinds) {
  const auto result = analyze(R"c(
    struct view { const char *s; int n; };
    struct view *g;
    struct view keep(struct view v) { return v; }
    const char *escape(void) {
      char local[8];
      struct view a = { local, 8 };
      struct view b = a;
      return b.s;
    }
    void self(struct view *v) { *v = *v; use(v->s); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            Strings{"9: 'b.s' may outlive 'local', which it points to"});
}

} // namespace
} // namespace weavec::analysis
