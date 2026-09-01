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

// -- Opaque values (documented holes) -----------------------------------------

TEST(Dataflow, PointerArithmeticIsOpaque) {
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
  EXPECT_TRUE(result.diagnostics.empty());
}

} // namespace
} // namespace weavec::analysis
