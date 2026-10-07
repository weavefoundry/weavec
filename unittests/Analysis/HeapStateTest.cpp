//===- HeapStateTest.cpp - Interprocedural heap state (RFC 0013) ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include <gtest/gtest.h>

namespace weavec::analysis {

using weavec::test::analyze;
using weavec::test::ids;
using Strings = std::vector<std::string>;

/// `<outcome>[/<reason>]` of `facet` at the site whose ledger text is
/// `text`, or empty when there is none.
static std::string outcomeAt(const test::AnalysisResult &result,
                             std::string_view text, core::Facet facet) {
  for (const core::UnitLedger &unit : result.ledger.units)
    for (const core::FunctionLedger &function : unit.functions)
      for (const core::Site &site : function.sites) {
        const core::FacetRecord *record = site.facet(facet);
        if (site.text != text || record == nullptr)
          continue;
        std::string out(core::toString(record->outcome()));
        if (!record->decision.reasonText().empty())
          out += "/" + std::string(record->decision.reasonText());
        return out;
      }
  return {};
}

/// No diagnostic is an error (a definite finding).
static bool noErrors(const test::AnalysisResult &result) {
  for (const core::Diagnostic &diagnostic : result.diagnostics.diagnostics())
    if (diagnostic.severity == core::Severity::Error)
      return false;
  return true;
}

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
  // `null_field` dereferences the null `empty` leaves; `nullable_field`
  // dereferences an allocation `maybe` did not test (RFC 0030 §3.2).
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"null-dereference", "allocation-failure"}));
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
  // RFC 0033 §2: a declared raw value stays raw through a summary's field.
  const auto result = analyze(R"c(
    #define RAW __attribute__((annotate("weavec.raw")))
    struct box { char *data; };
    struct box *make(char *RAW bits) {
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = bits; return b;
    }
    void bad(char *RAW bits) {
      struct box *b = make(bits); if (!b) return;
      b->data[0] = 0; free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{"unsafe-operation"});
}

TEST(HeapState, IntegerConvertedFieldIsNotRaw) {
  // RFC 0033 §2: a pointer converted from an integer is not raw, through a
  // summary's field too.
  const auto result = analyze(R"c(
    struct box { char *data; };
    struct box *make(unsigned long bits) {
      struct box *b = malloc(sizeof *b); if (!b) return NULL;
      b->data = (char *)bits; return b;
    }
    void ok(unsigned long bits) {
      struct box *b = make(bits); if (!b) return;
      b->data[0] = 0; free(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), Strings{});
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
  // RFC 0031 *Entry tests*: `ensure` publishes `g` when `g` was null at
  // entry, which the second call's `g` is not.
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
  // RFC 0031 *Entry tests*: `set` publishes `g` exactly when `g` was null
  // at entry, but its store to `g->data` runs after that join, and the exit
  // that failed to allocate a new box stores nothing, so no entry test
  // separates it: the store is possible, and the access may reach the old,
  // freed data of unknown extent (KNOWN-DIFFERENCES.md, *Unit tests*).
  EXPECT_EQ(outcomeAt(result, "g->data[4]", core::Facet::Spatial),
            "unresolved/unknown-index");
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(test::messages(result.diagnostics));
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
  // RFC 0031 §6.1: `get`'s returned `p` is `g` after a possible publication
  // (the entry value or a new box), which no value of the summary spells, so
  // the caller cannot tell `a.p` is `g`: the use is not proven, but no
  // longer a definite use after free (KNOWN-DIFFERENCES.md, *Unit tests*).
  EXPECT_EQ(outcomeAt(result, "g->data[0]", core::Facet::Temporal),
            "unresolved/may-alias-released");
  EXPECT_TRUE(noErrors(result));
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

} // namespace weavec::analysis
