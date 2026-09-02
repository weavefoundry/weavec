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
  EXPECT_EQ(messages(result.diagnostics), (Strings{"6: 'p' is freed twice"}));
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

TEST(Dataflow, ArrayElementsShareOneSummaryPlace) {
  // Documented imprecision: arr[0] and arr[1] are the same place `arr[*]`.
  const auto result = analyze(R"c(
    void f(void) {
      int *arr[4];
      arr[0] = malloc(4);
      arr[1] = malloc(4);
      free(arr[0]);
      use(arr[1]);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: use of 'arr[*]' after it was freed"}));
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
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
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
            (Strings{std::string(core::diag::UseAfterMove)}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: use of 'p' after it was moved"}));
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
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: use of 'p' after it was moved"}));
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

TEST(Dataflow, TwoMutableBorrowsConflict) {
  const auto result = analyze(R"c(
    void f(void) {
      int x = 0;
      int *a = &x;
      int *b = &x;
      use(a); use(b);
    }
  )c");
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
  const auto result = analyze(R"c(
    void f(void) {
      int x = 0;
      const int *a = &x;
      int *b = &x;
      use((void *)a); use(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot borrow 'x' as mutable because it is already "
                     "borrowed"}));
}

TEST(Dataflow, MutableThenSharedConflicts) {
  const auto result = analyze(R"c(
    void f(void) {
      int x = 0;
      int *a = &x;
      const int *b = &x;
      use(a); use((void *)b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot borrow 'x' as shared because it is already "
                     "mutably borrowed"}));
}

TEST(Dataflow, WritingABorrowedObjectConflicts) {
  const auto result = analyze(R"c(
    void f(void) {
      int x = 0;
      int *a = &x;
      x = 1;
      use(a);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot assign to 'x' while it is borrowed"}));
  EXPECT_EQ(notes(result.diagnostics), (Strings{"borrowed by 'a' here"}));
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
  const auto result = analyze(R"c(
    void f(void) {
      int x = 0;
      peek(&x);
      poke(&x);        /* fine: the temporary borrows ended */
      int *a = &x;
      poke(&x);
      use(a);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: cannot borrow 'x' as mutable because it is already "
                     "borrowed"}));
}

TEST(Dataflow, ArrayDecayBorrowsTheElements) {
  const auto result = analyze(R"c(
    void f(void) {
      int a[4];
      int *p = a;
      int *q = &a[1];
      use(p); use(q);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: cannot borrow 'a[*]' as mutable because it is already "
                     "borrowed"}));
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

} // namespace
} // namespace weavec::analysis
