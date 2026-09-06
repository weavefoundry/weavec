//===- HeapStateTest.cpp - Interprocedural heap state (RFC 0013) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/SummaryIO.h"

#include <gtest/gtest.h>

namespace weavec::analysis {

using weavec::test::analyze;
using weavec::test::ids;
using Strings = std::vector<std::string>;

static constexpr const char *Box = R"c(
  struct box { char *data; };
  struct box *make(void) {
    struct box *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->data = malloc(4);
    if (!b->data) { free(b); return NULL; }
    return b;
  }
)c";

TEST(HeapState, ConstructorPreservesChildBoundsAndOwnership) {
  const auto result = analyze(std::string(Box) + R"c(
    void overflow(void) {
      struct box *b = make();
      if (!b) return;
      b->data[4] = 0;
      free(b->data); free(b);
    }
    void leak(void) {
      struct box *b = make();
      if (b) free(b);
    }
    void clean(void) {
      struct box *b = make();
      if (!b) return;
      b->data[3] = 0;
      free(b->data); free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), (Strings{"out-of-bounds", "leak"}));
  const auto *summary = result.summary("make");
  ASSERT_NE(summary, nullptr);
  ASSERT_TRUE(summary->heap.contains(core::SummaryPath::result()));
  const auto &graph = summary->heap.at(core::SummaryPath::result());
  ASSERT_EQ(graph.fields.size(), 1U);
  EXPECT_EQ(graph.fields.begin()->value.extent,
            core::PathAffine::ofConstant(4));
  EXPECT_TRUE(graph.valid());
}

TEST(HeapState, ReturnedArgumentAliasRetainsIdentity) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    struct box *wrap(char *p) {
      struct box *b = malloc(sizeof *b);
      if (!b) return NULL;
      b->data = p; return b;
    }
    void bad(void) {
      char *p = malloc(4); if (!p) return;
      struct box *b = wrap(p);
      if (!b) { free(p); return; }
      free(p); b->data[0] = 0; free(b);
    }
    void good(void) {
      char *p = malloc(4); if (!p) return;
      struct box *b = wrap(p);
      if (!b) { free(p); return; }
      b->data[0] = 0; free(p); free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"use-after-free"});
}

TEST(HeapState, IndependentCallsAndSharedChild) {
  const auto result = analyze(R"c(
    struct pair { char *a, *b; struct pair *self; };
    struct pair *make(void) {
      struct pair *p = malloc(sizeof *p); if (!p) return NULL;
      p->a = malloc(8); if (!p->a) { free(p); return NULL; }
      p->b = p->a; p->self = p; return p;
    }
    void good(void) {
      struct pair *a = make(), *b = make();
      if (a) { a->b[7] = 0; free(a->a); free(a); }
      if (b) { b->a[7] = 0; free(b->b); free(b); }
    }
    void bad(void) {
      struct pair *p = make(); if (!p) return;
      free(p->a); p->b[0] = 0; free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"use-after-free"});
  const auto *summary = result.summary("make");
  ASSERT_NE(summary, nullptr);
  const auto &graph = summary->heap.at(core::SummaryPath::result());
  EXPECT_TRUE(graph.valid());
  EXPECT_LE(graph.fields.size(), 3U);
}

TEST(HeapState, AllocationSizeUsesItsOldValue) {
  const auto result = analyze(R"c(
    void constant(void) {
      size_t n = 4; char *p = malloc(n); if (!p) return;
      n = 8; p[7] = 0; free(p);
    }
    void symbolic(size_t n) {
      size_t original = n;
      char *p = malloc(n); if (!p) return;
      n = 2; p[original] = 0; free(p);
    }
    void good(size_t n) {
      size_t original = n;
      char *p = malloc(n); if (!p) return;
      n = 2;
      for (size_t i = 0; i < original; ++i) p[i] = 0;
      free(p);
    }
    void make_buffer(char **out, size_t *size) {
      *size = 4; *out = malloc(*size);
    }
    void output(void) {
      char *p; size_t n; make_buffer(&p, &n);
      if (!p) return;
      p[4] = 0; free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"out-of-bounds", "out-of-bounds", "out-of-bounds"}));
}

TEST(HeapState, NestedObjectsAndPublishedLocalAliases) {
  const auto result = analyze(R"c(
    struct leaf { char *data; };
    struct tree { struct leaf *child; };
    void make(struct tree **out) {
      struct tree *t = malloc(sizeof *t); *out = t;
      if (!t) return;
      t->child = malloc(sizeof *t->child);
      if (!t->child) { free(t); *out = NULL; return; }
      t->child->data = malloc(4);
    }
    void good(void) {
      struct tree *t; make(&t); if (!t) return;
      if (t->child->data) t->child->data[3] = 0;
      free(t->child->data); free(t->child); free(t);
    }
    void bad(void) {
      struct tree *t; make(&t); if (!t) return;
      if (t->child->data) t->child->data[4] = 0;
      free(t->child->data); free(t->child); free(t);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, FinalNullAndNullableFields) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    struct box *empty(void) {
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = malloc(4); free(b->data); b->data = NULL; return b;
    }
    struct box *maybe(void) {
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = malloc(4); return b;
    }
    void null_field(void) {
      struct box *b = empty(); if (!b) return;
      b->data[0] = 0; free(b);
    }
    void nullable_field(void) {
      struct box *b = maybe(); if (!b) return;
      b->data[0] = 0; free(b->data); free(b);
    }
    void good(void) {
      struct box *b = maybe(); if (!b) return;
      if (b->data) b->data[0] = 0;
      free(b->data); free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"null-dereference", "null-dereference"}));
}

TEST(HeapState, StringsCrossConstructorsAndForwarders) {
  const auto result = analyze(R"c(
    char *strcpy(char *, const char *);
    void *memset(void *, int, size_t);
    size_t strlen(const char *);
    struct box { char *data; };
    struct box *make(void) {
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = malloc(8);
      if (!b->data) { free(b); return NULL; }
      strcpy(b->data, "hello"); return b;
    }
    struct box *forward(void) { return make(); }
    char *unterm(void) {
      char *p = malloc(4); if (!p) return NULL;
      memset(p, 'a', 4); return p;
    }
    void bad(void) {
      struct box *b = forward(); if (!b) return;
      char dst[4]; strcpy(dst, b->data);
      free(b->data); free(b);
    }
    void unterminated(void) {
      char *p = unterm(); if (!p) return;
      (void)strlen(p); free(p);
    }
    void good(void) {
      struct box *b = forward(); if (!b) return;
      char dst[6]; strcpy(dst, b->data);
      free(b->data); free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"out-of-bounds", "out-of-bounds"}));
}

TEST(HeapState, ReplacementPreservesOldAliasesAndFinalFields) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    void reset(struct box *b) { free(b->data); b->data = NULL; }
    void replace(struct box *b) { free(b->data); b->data = malloc(8); }
    void old_alias(void) {
      struct box b; b.data = malloc(4); if (!b.data) return;
      char *old = b.data; replace(&b);
      old[0] = 0;
      if (b.data) b.data[7] = 0;
      free(b.data);
    }
    void good(void) {
      struct box b; b.data = malloc(4); if (!b.data) return;
      reset(&b); free(b.data);
    }
    struct box *extract(struct box *b) {
      char *old = b->data; b->data = NULL;
      struct box *r = malloc(sizeof *r);
      if (!r) { b->data = old; return NULL; }
      r->data = old; return r;
    }
    void incoming(void) {
      struct box b; b.data = malloc(4); if (!b.data) return;
      char *old = b.data;
      struct box *r = extract(&b);
      if (!r) { free(b.data); return; }
      free(old); r->data[0] = 0; free(r);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"use-after-free", "use-after-free"}));
}

TEST(HeapState, CrossUnitConstructorKeepsGraph) {
  const auto library = analyze(Box);
  ASSERT_TRUE(library.ast);
  ProgramDatabase database;
  database.add(library.analyzer->exports());
  const auto caller = weavec::test::analyzeInProgram(R"c(
    struct box { char *data; };
    struct box *make(void);
    void bad(void) {
      struct box *b = make(); if (!b) return;
      b->data[4] = 0; free(b->data); free(b);
    }
    void good(void) {
      struct box *b = make(); if (!b) return;
      b->data[3] = 0; free(b->data); free(b);
    }
  )c",
                                                     &database);
  ASSERT_TRUE(caller.ast);
  EXPECT_EQ(ids(caller.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, EscapingLocalBorrowAndReleaseFamilies) {
  const auto result = analyze(R"c(
    struct file;
    struct file *fopen(const char *, const char *);
    int fclose(struct file *);
    struct box { char *data; struct file *file; };
    struct box *local_borrow(void) {
      char local[4];
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = local; b->file = NULL; return b;
    }
    struct box *make(void) {
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = NULL; b->file = fopen("x", "r"); return b;
    }
    void wrong(void) {
      struct box *b = make(); if (!b) return;
      free(b->file); free(b);
    }
    void good(void) {
      struct box *b = make(); if (!b) return;
      if (b->file) fclose(b->file); free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"lifetime-too-short", "mismatched-release"}));
}

TEST(HeapState, ReusingSizeSnapshotInLoopNeverResizesOldObject) {
  const auto result = analyze(R"c(
    void good(size_t n, size_t count) {
      char *older = NULL;
      for (size_t k = 0; k < count; ++k) {
        char *p = malloc(n);
        size_t size = n;
        n = k + 1;
        if (p) {
          for (size_t i = 0; i < size; ++i) p[i] = 0;
        }
        free(older); older = p;
      }
      free(older);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{});
}

TEST(HeapState, FailureRetainsTheIncomingPointerAndItsExtent) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    int replace(struct box *b, size_t n) {
      char *fresh = malloc(n); if (!fresh) return 0;
      free(b->data); b->data = fresh; return 1;
    }
    void good(void) {
      struct box b; b.data = malloc(4); if (!b.data) return;
      if (!replace(&b, 8)) {
        b.data[3] = 0; free(b.data); return;
      }
      b.data[7] = 0; free(b.data);
    }
    void bad(void) {
      struct box b; b.data = malloc(4); if (!b.data) return;
      if (!replace(&b, 8)) {
        b.data[4] = 0; free(b.data); return;
      }
      free(b.data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, CopiesOfConstructorResultsKeepSharedFields) {
  const auto result = analyze(R"c(
    struct pair { char *a, *b; };
    struct pair *make(void) {
      struct pair *p = malloc(sizeof *p); if (!p) return NULL;
      p->a = malloc(4); if (!p->a) { free(p); return NULL; }
      p->b = p->a;
      struct pair *q = p; return q;
    }
    struct pair *again(void) {
      struct pair *a = make();
      struct pair *b = a; return b;
    }
    void good(void) {
      struct pair *p = again(); if (!p) return;
      free(p->a); free(p);
    }
    void bad(void) {
      struct pair *p = again(); if (!p) return;
      free(p->a); p->b[0] = 0; free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"use-after-free"});
}

TEST(HeapState, RawFieldStaysRaw) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    struct box *make(unsigned long bits) {
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = (char *)bits; return b;
    }
    void bad(unsigned long bits) {
      struct box *b = make(bits); if (!b) return;
      b->data[0] = 0; free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"unsafe-operation"});
}

TEST(HeapState, RecursiveProjectionIsBoundedAndMarkedIncomplete) {
  const auto result = analyze(R"c(
    struct node { struct node *next; char *data; };
    struct node *make(unsigned depth) {
      struct node *n = malloc(sizeof *n); if (!n) return NULL;
      n->data = malloc(4);
      n->next = depth ? make(depth - 1) : NULL;
      return n;
    }
    struct node *forward(unsigned depth) { return make(depth); }
    struct node *local(unsigned depth) { struct node *p = make(depth), *q = p; return q; }
  )c");
  ASSERT_TRUE(result.ast);
  for (const char *name : {"make", "forward", "local"}) {
    const auto *summary = result.summary(name);
    ASSERT_NE(summary, nullptr);
    ASSERT_TRUE(summary->heap.contains(core::SummaryPath::result()));
    const auto &graph = summary->heap.at(core::SummaryPath::result());
    EXPECT_TRUE(graph.incomplete) << name;
    EXPECT_TRUE(graph.valid()) << name;
    EXPECT_LE(graph.fields.size(), core::MaxHeapFields);
    for (const auto &field : graph.fields)
      EXPECT_LE(field.dest.steps.size(), core::MaxHeapPathDepth);
  }
}

TEST(HeapState, RecordResultsAndCopiesPreserveSharedChildBounds) {
  const auto result = analyze(R"c(
    struct pair { char *a, *b; };
    struct pair make(void) {
      struct pair p; p.a = malloc(4); p.b = p.a; return p;
    }
    struct pair forwarded(void) { return make(); }
    struct pair again(void) { struct pair p = forwarded(), q = p; return q; }
    void good(void) {
      struct pair a = again(), b = a;
      if (b.b) b.b[3] = 0;
      free(b.a);
    }
    void overflow(void) {
      struct pair a = again(), b = a;
      if (b.b) b.b[4] = 0;
      free(b.a);
    }
    void stale(void) {
      struct pair a = again(), b = a;
      if (!b.b) return;
      free(b.a); b.b[0] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"out-of-bounds", "use-after-free"}));
}

TEST(HeapState, AssignmentAndConditionalConstructorKeepChildBounds) {
  const auto result = analyze(std::string(Box) + R"c(
    void good(void) {
      struct box *p; p = make(); if (!p) return;
      p->data[3] = 0; free(p->data); free(p);
    }
    void bad(int flag) {
      struct box *p; p = flag ? make() : NULL; if (!p) return;
      p->data[4] = 0; free(p->data); free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, SeveralOutputsAndTheReturnShareOneObjectGraph) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    struct box *publish(struct box **a, struct box **b) {
      struct box *p = malloc(sizeof *p);
      if (p) p->data = malloc(4);
      *a = p; *b = p; return p;
    }
    void good(void) {
      struct box *a, *b, *c = publish(&a, &b); if (!c) return;
      if (b->data) b->data[3] = 0;
      free(a->data); free(c);
    }
    void bad(void) {
      struct box *a, *b, *c = publish(&a, &b); if (!c) return;
      if (b->data) b->data[4] = 0;
      free(a->data); free(c);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
  const auto *summary = result.summary("publish");
  ASSERT_NE(summary, nullptr);
  const auto printed =
      core::printSummary(*summary, [](std::uint32_t) { return "g"; });
  std::string error;
  EXPECT_TRUE(core::parseSummary(
      printed, [](std::string_view) { return 0U; }, &error))
      << error << printed;
}

TEST(HeapState, APublishedRootAndItsExplicitChildStoreAllocateOnce) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    void make(struct box **out) {
      *out = malloc(sizeof **out); if (!*out) return;
      (*out)->data = malloc(4);
    }
    void good(void) {
      struct box *b; make(&b); if (!b) return;
      if (b->data) b->data[3] = 0;
      free(b->data); free(b);
    }
    void bad(void) {
      struct box *b; make(&b); if (!b) return;
      if (b->data) b->data[4] = 0;
      free(b->data); free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, DerivedChildrenKeepTheirOffsetsAndObjectIdentity) {
  const auto result = analyze(R"c(
    struct box { char *base; char *cursor; };
    struct box *make(void) {
      struct box *p = malloc(sizeof *p); if (!p) return 0;
      p->base = malloc(8);
      if (!p->base) { free(p); return 0; }
      p->cursor = p->base;
      p->cursor++;
      return p;
    }
    void good(void) {
      struct box *p = make(); if (!p) return;
      p->cursor[6] = 0;
      free(p->base); free(p);
    }
    void bad(void) {
      struct box *p = make(); if (!p) return;
      p->cursor[7] = 0;
      free(p->base); free(p);
    }
    char *advance(char *p) { char *q = p; q++; return q; }
    void borrowed(void) {
      char *p = malloc(8); if (!p) return;
      char *q = advance(p); q[7] = 0; free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"out-of-bounds", "out-of-bounds"}));
}

TEST(HeapState, LazyPublicationKeepsItsEntryGuardAcrossTheCall) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    static struct box *g;
    void ensure(void) {
      if (g) return;
      g = malloc(sizeof *g);
      if (g) g->data = malloc(4);
    }
    void bad(void) {
      ensure(); if (!g || !g->data) return;
      free(g->data); ensure(); g->data[0] = 0;
      free(g); g = 0;
    }
    void good(void) {
      ensure(); if (!g || !g->data) return;
      ensure(); g->data[3] = 0;
      free(g->data); free(g); g = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"use-after-free"});
}

TEST(HeapState, AFieldWriteAfterConditionalPublicationStillRuns) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    static struct box *g;
    void set(void) {
      if (!g) g = malloc(sizeof *g);
      if (g) g->data = malloc(4);
    }
    void good(void) {
      set(); if (!g || !g->data) return;
      free(g->data); set(); if (!g->data) return;
      g->data[3] = 0; free(g->data); free(g); g = 0;
    }
    void bad(void) {
      set(); if (!g || !g->data) return;
      free(g->data); set(); if (!g->data) return;
      g->data[4] = 0; free(g->data); free(g); g = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, ReturningAnExtractedPointerUsesTheEntryCell) {
  const auto result = analyze(R"c(
    char *extract_pointer(char **slot) {
      char *p = *slot; if (!p) return 0; *slot = 0; return p;
    }
    char *forward(char **slot) { return extract_pointer(slot); }
    void good(void) {
      char *p = malloc(4); if (!p) return;
      char *q = forward(&p); q[3] = 0; free(q);
    }
    void bad(void) {
      char *p = malloc(4); if (!p) return;
      char *old = p; char *q = forward(&p);
      free(old); q[0] = 0;
    }
    void leak(void) {
      char *p = malloc(4); if (!p) return;
      char *q = forward(&p); q[0] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), (Strings{"use-after-free", "leak"}));
}

TEST(HeapState, AReturnedRecordSharesAPublishedGlobalObject) {
  const auto result = analyze(R"c(
    struct box { char *data; }; static struct box *g;
    void ensure(void) {
      if (g) return;
      g = malloc(sizeof *g); if (g) g->data = malloc(4);
    }
    struct pair { struct box *p; };
    struct pair get(void) {
      ensure(); struct pair a = {g}; struct pair b = a; return b;
    }
    void bad(void) {
      struct pair a = get(); if (!a.p || !a.p->data) return;
      free(a.p->data); ensure(); g->data[0] = 0;
      free(g); g = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"use-after-free"});
}

TEST(HeapState, ATraversalAfterPublicationDoesNotGuardTheEarlierWrite) {
  const auto result = analyze(R"c(
    struct node { struct node *next; };
    struct box { char *data; };
    void reset(struct box *b, struct node *head) {
      b->data = malloc(4);
      for (struct node *n = head; n; n = n->next) {}
    }
    void good(void) {
      struct node n = {0}; struct box b = {malloc(4)};
      free(b.data); reset(&b, &n);
      if (b.data) b.data[3] = 0;
      free(b.data);
    }
    void bad(void) {
      struct node n = {0}; struct box b = {malloc(4)};
      free(b.data); reset(&b, &n);
      if (b.data) b.data[4] = 0;
      free(b.data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, SwapBasedReplacementPreservesBothEntryValues) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    void swap(struct box *a, struct box *b) {
      char *old = a->data; a->data = b->data; b->data = old;
    }
    void reset(struct box *b) {
      struct box n = {malloc(4)};
      swap(b, &n); free(n.data);
    }
    void good(void) {
      struct box b = {malloc(8)};
      reset(&b); if (b.data) b.data[3] = 0;
      free(b.data);
    }
    void bad(void) {
      struct box b = {malloc(8)};
      reset(&b); if (b.data) b.data[4] = 0;
      free(b.data);
    }
    void stale(void) {
      struct box b = {malloc(8)}; if (!b.data) return;
      char *old = b.data;
      reset(&b); old[0] = 0; free(b.data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"out-of-bounds", "use-after-free"}));
}

TEST(HeapState, KnownFinalValuesSurviveWidenedConsumptionEffects) {
  const auto library = analyze(R"c(
    struct box { char *data; };
    void reset(struct box *b) { free(b->data); b->data = malloc(4); }
  )c");
  ASSERT_TRUE(library.ast);
  auto exports = library.analyzer->exports();
  auto &summary = exports.functions.at("reset").summary;
  const auto path = core::SummaryPath::param(0).deref().field("data");
  // A recursive join can lose the legacy must-replaced flag while retaining
  // an independently known final value. The old input is still consumed.
  summary.effects.at(path).replaced = false;
  ProgramDatabase database;
  database.add(exports);
  const auto caller = weavec::test::analyzeInProgram(R"c(
    struct box { char *data; }; void reset(struct box *);
    void good(void) {
      struct box b = {malloc(8)}; reset(&b);
      if (b.data) b.data[3] = 0; free(b.data);
    }
    void stale(void) {
      struct box b = {malloc(8)}; if (!b.data) return;
      char *old = b.data; reset(&b); old[0] = 0; free(b.data);
    }
  )c",
                                                     &database);
  ASSERT_TRUE(caller.ast);
  EXPECT_EQ(ids(caller.diagnostics), Strings{"use-after-free"});
}

TEST(HeapState, ConsumptionOfCopiedInputsDoesNotConsumeOldDestinations) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    void put(struct box *b, char *p) { b->data = p; }
    void helper(struct box *a, struct box *b) {
      put(b, a->data); free(a->data);
    }
    void good(void) {
      struct box a = {malloc(4)}, b = {malloc(8)};
      char *old = b.data; helper(&a, &b); free(old);
    }
    void bad(void) {
      struct box a = {malloc(4)}, b = {malloc(8)};
      char *old = b.data; helper(&a, &b); free(old); free(b.data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"double-free"});
  const auto *summary = result.summary("helper");
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(
      summary->effectOf(core::SummaryPath::param(0).deref().field("data"))
          .consumed());
  EXPECT_FALSE(
      summary->effectOf(core::SummaryPath::param(1).deref().field("data"))
          .consumed());
}

TEST(HeapState, LocalAliasSwapsSnapshotBothIncomingCells) {
  const auto result = analyze(R"c(
    struct box { char *data; };
    void swap(struct box *a, struct box *b) {
      struct box *x = a, *y = b;
      char *old = x->data; x->data = y->data; y->data = old;
    }
    void good(void) {
      struct box a = {malloc(4)}, b = {malloc(8)};
      swap(&a, &b);
      if (a.data) a.data[7] = 0;
      if (b.data) b.data[3] = 0;
      free(a.data); free(b.data);
    }
    void bad(void) {
      struct box a = {malloc(4)}, b = {malloc(8)};
      swap(&a, &b); if (b.data) b.data[4] = 0;
      free(a.data); free(b.data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, ConstructorCleanupDoesNotConsumeOldCallerFields) {
  const auto result = analyze(R"c(
    struct inner { char *data, *next; char area[8]; };
    struct box { struct inner *state; };
    void reset(struct box *b) { b->state->next = b->state->area; }
    int create(struct box *b, int fail) {
      struct inner *p = malloc(sizeof *p); if (!p) return -1;
      b->state = p; p->data = NULL; reset(b);
      if (fail) { free(p); b->state = NULL; return -1; }
      return 0;
    }
    void good(void) {
      struct box b = {NULL};
      if (create(&b, 0)) return;
      free(b.state);
    }
    void incoming(struct box *b, char *p) {
      b->state = malloc(sizeof *b->state); if (!b->state) return;
      b->state->data = p; free(b->state->data);
      free(b->state); b->state = NULL;
    }
    void maybe_existing(struct box *b, int replace) {
      if (replace) {
        b->state = malloc(sizeof *b->state); if (!b->state) return;
        b->state->data = NULL;
      }
      free(b->state->data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
  const auto child =
      core::SummaryPath::param(0).deref().field("state").deref().field("next");
  EXPECT_FALSE(result.summary("create")->effectOf(child).consumed());
  EXPECT_TRUE(result.summary("incoming")->consumes(1));
  EXPECT_TRUE(
      result.summary("maybe_existing")
          ->effectOf(
              core::SummaryPath::param(0).deref().field("state").deref().field(
                  "data"))
          .consumed());
}

TEST(HeapState, WritesThroughLocalAndEmbeddedAliasesAreFinalOutputs) {
  const auto result = analyze(R"c(
    struct box { char *data; }; struct outer { struct box box; };
    void initialize(struct outer *out) {
      struct box *b = &out->box; b->data = malloc(4);
    }
    void replace(struct outer *out) {
      struct outer *local = out; free(local->box.data); initialize(local);
    }
    void good(void) {
      struct outer out; initialize(&out); replace(&out);
      if (out.box.data) out.box.data[3] = 0;
      free(out.box.data);
    }
    void bad(void) {
      struct outer out; initialize(&out);
      if (out.box.data) out.box.data[4] = 0;
      free(out.box.data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"out-of-bounds"});
}

TEST(HeapState, UnknownFinalValuesRetainNoIntermediateBound) {
  const auto library = analyze(R"c(
    struct box { char *data; };
    void reset(struct box *b) { b->data = malloc(4); }
  )c");
  ASSERT_TRUE(library.ast);
  auto exports = library.analyzer->exports();
  auto &summary = exports.functions.at("reset").summary;
  const auto path = core::SummaryPath::param(0).deref().field("data");
  summary.heap.at(path).fields.clear();
  summary.heap.at(path).addField(
      core::Store{.dest = core::SummaryPath::result(),
                  .value = core::ValueSource::unknown()});
  ProgramDatabase database;
  database.add(exports);
  const auto caller = weavec::test::analyzeInProgram(R"c(
    struct box { char *data; }; void reset(struct box *);
    void unknown_size(void) {
      struct box b = {0}; reset(&b);
      if (b.data) b.data[7] = 0; free(b.data);
    }
  )c",
                                                     &database);
  ASSERT_TRUE(caller.ast);
  EXPECT_EQ(ids(caller.diagnostics), Strings{});
}

TEST(HeapState, UnknownFinalValuesDoNotProveDisjointnessFromCopyAlternatives) {
  const auto library = analyze(R"c(
    void publish(char **out, char *p) { *out = p; }
  )c");
  ASSERT_TRUE(library.ast);
  auto exports = library.analyzer->exports();
  auto &summary = exports.functions.at("publish").summary;
  const auto path = core::SummaryPath::param(0).deref();
  summary.heap.at(path).fields.clear();
  summary.heap.at(path).addField(
      core::Store{.dest = core::SummaryPath::result(),
                  .value = core::ValueSource::unknown()});
  ProgramDatabase database;
  database.add(exports);
  const auto caller = weavec::test::analyzeInProgram(R"c(
    void publish(char **, char *);
    void bad(void) {
      char *p = malloc(4); if (!p) return;
      char *out; publish(&out, p); free(p); out[0] = 0;
    }
  )c",
                                                     &database);
  ASSERT_TRUE(caller.ast);
  EXPECT_EQ(ids(caller.diagnostics), Strings{"use-after-free"});
}

} // namespace weavec::analysis
