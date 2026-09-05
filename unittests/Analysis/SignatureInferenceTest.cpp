//===- SignatureInferenceTest.cpp - Tests for RFC 0003 --------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One test per behaviour promised by RFC 0003: what summaries are derived
// from bodies, what applying them at a call catches, what annotations are
// checked against, and what the boundary with unknown code reports. Line
// numbers count from the line after `R"c(` (see TestUtils.h).
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/TranslationUnitAnalysis.h"

#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using weavec::test::analyze;
using weavec::test::ids;
using weavec::test::messages;
using weavec::test::notes;

using core::SummaryPath;
using core::ValueSource;
using Strings = std::vector<std::string>;

// Kept on one line so the first line of each test body is still line 2.
constexpr const char *Types = "struct node { int v; struct node *next; }; "
                              "struct buf { char *data; size_t len; };";

// -- Deriving a summary (RFC 0003, "Deriving a summary") ----------------------

TEST(SignatureInference, FreeingWrapperConsumesItsParameter) {
  const auto result = analyze(std::string(Types) + R"c(
    static void node_free(struct node *n) { free(n); }
    static void node_drop(struct node *n) { take(n); }
  )c");
  ASSERT_TRUE(result.ast);
  const core::FunctionSummary *frees = result.summary("node_free");
  ASSERT_NE(frees, nullptr);
  EXPECT_TRUE(frees->consumes(0));
  EXPECT_TRUE(frees->frees(0));
  EXPECT_EQ(frees->inferredKind(0), core::OwnershipKind::Owned);

  const core::FunctionSummary *drops = result.summary("node_drop");
  ASSERT_NE(drops, nullptr);
  EXPECT_TRUE(drops->consumes(0));
  EXPECT_FALSE(drops->frees(0)) << "moved to `take`, not released";
}

TEST(SignatureInference, AllocatingWrapperReturnsFresh) {
  const auto result = analyze(std::string(Types) + R"c(
    static struct node *node_new(int v) {
      struct node *n = malloc(sizeof *n);
      if (n) { n->v = v; n->next = NULL; }
      return n;
    }
    static struct node *maybe(int c) { return c ? malloc(8) : NULL; }
  )c");
  ASSERT_TRUE(result.ast);
  const core::FunctionSummary *s = result.summary("node_new");
  ASSERT_NE(s, nullptr);
  // `malloc` may fail and the null path merges back before the return, so
  // the wrapper may return null too (RFC 0008, *Nullness*).
  // The allocation carries its extent, `sizeof(struct node)` (RFC 0011).
  EXPECT_EQ(
      s->returns,
      (std::set<ValueSource>{
          ValueSource::freshAt("free", {}, core::PathAffine::ofConstant(16)),
          ValueSource::null()}));
  EXPECT_TRUE(s->mayReturnNull());
  EXPECT_EQ(s->inferredReturnKind(), core::OwnershipKind::Owned);

  const core::FunctionSummary *m = result.summary("maybe");
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(
      m->returns,
      (std::set<ValueSource>{
          ValueSource::freshAt("free", {}, core::PathAffine::ofConstant(8)),
          ValueSource::null()}));
  EXPECT_EQ(m->inferredReturnKind(), core::OwnershipKind::Owned);
}

TEST(SignatureInference, ReadsAndWritesThroughParameters) {
  const auto result = analyze(std::string(Types) + R"c(
    static int get(const struct node *n) { return n->v; }
    static void set(struct node *n, int v) { n->v = v; }
    static void bump(struct node *n) { n->v++; }
    static int ptr_only(struct node *n) { return n != NULL; }
    static int via_alias(struct node *n) { struct node *m = n; return m->v; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(result.summary("get")->borrowKind(0), core::BorrowKind::Shared);
  EXPECT_EQ(result.summary("get")->inferredKind(0),
            core::OwnershipKind::Shared);
  EXPECT_EQ(result.summary("set")->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(result.summary("bump")->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_EQ(result.summary("ptr_only")->borrowKind(0), std::nullopt)
      << "comparing the pointer value does not touch the pointee";
  EXPECT_EQ(result.summary("via_alias")->borrowKind(0),
            core::BorrowKind::Shared)
      << "accesses through a copy count against the parameter";
}

TEST(SignatureInference, StoresThroughParameters) {
  const auto result = analyze(std::string(Types) + R"c(
    static void buf_init(struct buf *b, size_t n) {
      b->data = malloc(n);
      b->len = n;
    }
    static int make(char **out) { *out = malloc(8); return *out != NULL; }
    static void link(struct node *a, struct node *b) { a->next = b; }
    static void point(struct buf *b, char *p) { b->data = &p[0]; }
  )c");
  ASSERT_TRUE(result.ast);
  const SummaryPath b = SummaryPath::param(0);

  const core::FunctionSummary *init = result.summary("buf_init");
  ASSERT_NE(init, nullptr);
  // An unchecked `malloc` result: the store may be fresh or null (RFC 0008);
  // the fresh object is `n` bytes (RFC 0011).
  EXPECT_EQ(init->stores,
            (std::set<core::Store>{core::Store{.dest = b.deref().field("data"),
                                               .value = ValueSource::freshAt(
                                                   "free", {},
                                                   core::PathAffine::ofPath(
                                                       SummaryPath::param(1)))},
                                   core::Store{.dest = b.deref().field("data"),
                                               .value = ValueSource::null()}}));
  EXPECT_EQ(init->borrowKind(0), core::BorrowKind::Mutable);
  EXPECT_TRUE(init->effectOf(b.deref().field("len")).written);

  const core::FunctionSummary *make = result.summary("make");
  ASSERT_NE(make, nullptr);
  EXPECT_TRUE(make->stores.contains(core::Store{
      .dest = b.deref(),
      .value =
          ValueSource::freshAt("free", {}, core::PathAffine::ofConstant(8))}));

  const core::FunctionSummary *link = result.summary("link");
  ASSERT_NE(link, nullptr);
  EXPECT_EQ(link->stores,
            (std::set<core::Store>{core::Store{
                .dest = b.deref().field("next"),
                .value = ValueSource::copy(SummaryPath::param(1))}}));
}

TEST(SignatureInference, EscapesIntoGlobals) {
  const auto result = analyze(R"c(
    static char *g;
    static void keep(char *p) { g = p; }
    static void keep_alloc(void) { g = malloc(8); }
    static void drop(void) { free(g); }
    static void drop_null(void) { free(g); g = NULL; }
  )c");
  ASSERT_TRUE(result.ast);
  const core::FunctionSummary *keep = result.summary("keep");
  ASSERT_NE(keep, nullptr);
  ASSERT_EQ(keep->stores.size(), 1U);
  EXPECT_EQ(keep->stores.begin()->dest.root, core::SummaryRoot::Global);
  EXPECT_EQ(keep->stores.begin()->value,
            ValueSource::copy(SummaryPath::param(0)));

  const core::FunctionSummary *keepAlloc = result.summary("keep_alloc");
  ASSERT_NE(keepAlloc, nullptr);
  ASSERT_EQ(keepAlloc->stores.size(), 2U) << "fresh, or null when malloc fails";
  // RFC 0011: the allocation carries its extent.
  EXPECT_EQ(keepAlloc->stores.begin()->value,
            ValueSource::freshAt("free", {}, core::PathAffine::ofConstant(8)));
  EXPECT_EQ(std::next(keepAlloc->stores.begin())->value, ValueSource::null());

  const core::FunctionSummary *drop = result.summary("drop");
  ASSERT_NE(drop, nullptr);
  ASSERT_EQ(drop->effects.size(), 1U);
  EXPECT_EQ(drop->effects.begin()->first.root, core::SummaryRoot::Global);
  EXPECT_TRUE(drop->effects.begin()->second.freed);

  // Re-nulling a global: the caller's copies of the old value are dead, the
  // global itself holds the stored null (RFC 0008, *Replaced values*).
  const core::FunctionSummary *dropNull = result.summary("drop_null");
  ASSERT_NE(dropNull, nullptr);
  ASSERT_EQ(dropNull->effects.size(), 1U);
  EXPECT_TRUE(dropNull->effects.begin()->second.freed);
  EXPECT_TRUE(dropNull->effects.begin()->second.replaced);
  ASSERT_EQ(dropNull->stores.size(), 1U);
  EXPECT_EQ(dropNull->stores.begin()->value, ValueSource::null());
}

TEST(SignatureInference, ExitStateVersusEvents) {
  const auto result = analyze(std::string(Types) + R"c(
    static void destroy(struct buf *b) { free(b->data); b->data = NULL; }
    static void destroy_raw(struct buf *b) { free(b->data); }
    static void destroy_maybe(struct buf *b, int c) {
      free(b->data);
      if (c) b->data = NULL;
    }
    static void own_then_free(struct buf *b) { b->data = malloc(8); free(b->data); }
    static void free_and_null(char *p) { free(p); p = NULL; }
    static void repoint(struct node *p) { p = p->next; free(p->next); }
  )c");
  ASSERT_TRUE(result.ast);
  const SummaryPath data = SummaryPath::param(0).deref().field("data");

  // Caller memory (RFC 0008, *Replaced values*): the release is recorded as
  // it happened; the re-nulled field is `replaced`, since at no return does
  // it still hold the freed value.
  const core::FunctionSummary *destroy = result.summary("destroy");
  ASSERT_NE(destroy, nullptr);
  EXPECT_TRUE(destroy->effectOf(data).freed);
  EXPECT_TRUE(destroy->effectOf(data).replaced);
  EXPECT_TRUE(destroy->stores.contains(
      core::Store{.dest = data, .value = ValueSource::null()}));
  EXPECT_TRUE(result.summary("destroy_raw")->effectOf(data).freed);
  EXPECT_FALSE(result.summary("destroy_raw")->effectOf(data).replaced);
  // Re-nulled on one path only: the caller's place may still hold it.
  EXPECT_TRUE(result.summary("destroy_maybe")->effectOf(data).freed);
  EXPECT_FALSE(result.summary("destroy_maybe")->effectOf(data).replaced);
  // A value this function stored there itself is not the caller's.
  EXPECT_FALSE(result.summary("own_then_free")->effectOf(data).consumed());
  EXPECT_TRUE(result.summary("own_then_free")->effectOf(data).written);

  // The parameter variable is the callee's own: events govern, and the
  // reassignment is never `replaced`.
  const core::FunctionSummary *fan = result.summary("free_and_null");
  ASSERT_NE(fan, nullptr);
  EXPECT_TRUE(fan->frees(0));
  EXPECT_FALSE(fan->effectOf(SummaryPath::param(0)).replaced);

  // A re-pointed parameter falls back to events for its sub-paths.
  const core::FunctionSummary *repoint = result.summary("repoint");
  ASSERT_NE(repoint, nullptr);
  EXPECT_TRUE(
      repoint->effectOf(SummaryPath::param(0).deref().field("next")).freed);
}

TEST(SignatureInference, ReturnSources) {
  const auto result = analyze(std::string(Types) + R"c(
    static struct node *same(struct node *n) { return n; }
    static struct node *next_of(struct node *n) { return n->next; }
    static int *field_of(struct node *n) { return &n->v; }
    static struct node *pick(struct node *a, struct node *b, int c) { return c ? a : b; }
    static char *local_alias(struct node *n) { struct node *m = n; return (char *)m; }
    static char *owned_local(void) { char *p = malloc(4); return p; }
    static char *from_int(long x) { return (char *)x; }
  )c");
  ASSERT_TRUE(result.ast);
  const SummaryPath n = SummaryPath::param(0);
  EXPECT_EQ(result.summary("same")->returns,
            std::set<ValueSource>{ValueSource::copy(n)});
  EXPECT_EQ(result.summary("next_of")->returns,
            std::set<ValueSource>{ValueSource::copy(n.deref().field("next"))});
  // `&n->v` is a copy of `n` at the field `v` (RFC 0011, *Derived pointers*).
  EXPECT_EQ(result.summary("field_of")->returns,
            std::set<ValueSource>{ValueSource::copyAt(
                n, core::PointerOffset::ofField("struct node.v"))});
  EXPECT_EQ(result.summary("field_of")->inferredReturnKind(),
            core::OwnershipKind::Shared);
  EXPECT_EQ(result.summary("pick")->returns,
            (std::set<ValueSource>{ValueSource::copy(n),
                                   ValueSource::copy(SummaryPath::param(1))}));
  EXPECT_EQ(result.summary("local_alias")->returns,
            std::set<ValueSource>{ValueSource::copy(n)});
  EXPECT_EQ(
      result.summary("owned_local")->returns,
      (std::set<ValueSource>{
          ValueSource::freshAt("free", {}, core::PathAffine::ofConstant(4)),
          ValueSource::null()}))
      << "an unchecked malloc result may be null (RFC 0008)";
  // An integer cast yields a raw pointer (RFC 0004), and the summary says so.
  EXPECT_EQ(result.summary("from_int")->returns,
            std::set<ValueSource>{ValueSource::raw()});
  EXPECT_EQ(result.summary("from_int")->inferredReturnKind(),
            core::OwnershipKind::Raw);
}

TEST(SignatureInference, RecursionReachesAFixpoint) {
  const auto result = analyze(std::string(Types) + R"c(
    static void list_free(struct node *n) {
      if (!n) return;
      list_free(n->next);
      free(n);
    }
    static void even_free(struct node *n);
    static void odd_free(struct node *n) { if (n) even_free(n->next); free(n); }
    static void even_free(struct node *n) { if (n) odd_free(n->next); free(n); }
    static int count(const struct node *n) { return n ? 1 + count(n->next) : 0; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty());
  EXPECT_TRUE(result.summary("list_free")->frees(0));
  EXPECT_TRUE(result.summary("odd_free")->frees(0));
  EXPECT_TRUE(result.summary("even_free")->frees(0));
  EXPECT_EQ(result.summary("count")->inferredKind(0),
            core::OwnershipKind::Shared);
}

TEST(SignatureInference, UnsafeAndAnnotatedDefinitions) {
  // RFC 0004 supersedes RFC 0003 here: an unsafe body is analysed (silently)
  // and its summary is consulted by callers like any other.
  const auto result = analyze(std::string(Types) + R"c(
    UNSAFE static void unsafe_free(struct node *n) { free(n); free(n); }
    static void annotated(struct node *OWNED n) { use(n); }
    void f(struct node *a, struct node *b) {
      unsafe_free(a);
      use(a);           /* the unsafe body is consulted */
      annotated(b);
      use(b);           /* the annotation is */
    }
  )c");
  ASSERT_TRUE(result.ast);
  ASSERT_NE(result.summary("unsafe_free"), nullptr);
  EXPECT_TRUE(result.summary("unsafe_free")->frees(0));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"3: 'n' is leaked", "6: use of 'a' after it was freed",
                     "8: use of 'b' after it was moved"}))
      << "the double free inside the unsafe body is not reported; the owned "
         "parameter `annotated` never releases is (RFC 0007)";
}

TEST(SignatureInference, UnsafeDeclarationWithoutBodyIsAnEmptySummary) {
  // RFC 0003: `WEAVEC_UNSAFE` on a bodyless declaration asks for the empty
  // summary rather than the boundary warning.
  const auto result = analyze(R"c(
    UNSAFE void vouched(void *p);
    void f(char *p) { vouched(p); use(p); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty()) << messages(result.diagnostics)[0];
}

// -- Applying a summary at a call (RFC 0003, "Soundness") ---------------------

TEST(SignatureInference, UseAfterFreeThroughWrapper) {
  const auto result = analyze(std::string(Types) + R"c(
    static void node_free(struct node *n) { free(n); }
    static struct node *node_new(void) { return malloc(sizeof(struct node)); }
    int f(void) {
      struct node *n = node_new();
      node_free(n);
      return n->v;
    }
    void g(void) {
      struct node *n = node_new();
      node_free(n);
      free(n);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"7: use of 'n' after it was freed", "12: 'n' is freed twice"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"freed here"}));
  EXPECT_EQ(result.diagnostics.diagnostics()[0].notes[0].location.line, 6U)
      << "the note points at the call that freed it";
  EXPECT_EQ(notes(result.diagnostics, 1), (Strings{"previously freed here"}));
}

TEST(SignatureInference, WrapperOfWrapperAndMutualRecursion) {
  const auto result = analyze(std::string(Types) + R"c(
    static void node_free(struct node *n) { free(n); }
    static void node_free2(struct node *n) { node_free(n); }
    static void node_free3(struct node *n) { if (n) node_free2(n); }
    static void even_free(struct node *n);
    static void odd_free(struct node *n) { if (n) even_free(n->next); free(n); }
    static void even_free(struct node *n) { if (n) odd_free(n->next); free(n); }
    void f(struct node *a, struct node *b) {
      node_free3(a);
      use(a);
      odd_free(b);
      use(b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"10: use of 'a' after it was freed",
                     "12: use of 'b' after it was freed"}));
}

TEST(SignatureInference, FieldFreedByCallee) {
  const auto result = analyze(std::string(Types) + R"c(
    static void buf_init(struct buf *b, size_t n) { b->data = malloc(n); }
    static void buf_destroy(struct buf *b) { free(b->data); }
    static void buf_destroy_null(struct buf *b) { free(b->data); b->data = NULL; }
    void by_pointer(struct buf *b) {
      buf_destroy(b);
      b->data[0] = 1;
    }
    void by_address(void) {
      struct buf b;
      buf_init(&b, 8);
      buf_destroy(&b);
      b.data[0] = 1;
    }
    void destroy_idiom(void) {
      struct buf b;
      buf_init(&b, 8);
      buf_destroy_null(&b);
      b.data = malloc(4);
      free(b.data);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: use of 'b->data' after it was freed",
                     "13: use of 'b.data' after it was freed"}));
}

TEST(SignatureInference, OutParameters) {
  const auto result = analyze(R"c(
    static int make(char **out) { *out = malloc(8); return *out != NULL; }
    static int alias_out(char **out, char *p) { *out = p; return 1; }
    void f(void) {
      char *s;
      if (!make(&s)) return;
      free(s);
      s[0] = 1;
    }
    void g(void) {
      char *s;
      if (!make(&s)) return;
      s[0] = 1;
      free(s);
    }
    void h(char *p) {
      char *q;
      alias_out(&q, p);
      free(p);
      use(q);
    }
    long strtol(const char *, char **, int);
    void i(void) {
      char *text = malloc(8); if (!text) return;
      char *end;
      strtol(text, &end, 10);
      free(text);
      use(end);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: use of 's' after it was freed",
                     "20: use of 'q' after it was freed",
                     "28: use of 'end' after it was freed"}));
}

TEST(SignatureInference, EscapeThroughCallee) {
  const auto result = analyze(R"c(
    static char *g;
    static void keep(char *p) { g = p; }
    static void store_in(char **slot, char *p) { *slot = p; }
    void f(void) {
      char local[8];
      keep(local);
    }
    void ok(char *outer) {
      keep(outer);
      keep(malloc(8));
    }
    void h(char **slot) {
      int x;
      store_in(slot, (char *)&x);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: 'g' may outlive 'local', which it points to",
                     "15: '*slot' may outlive 'x', which it points to"}));
}

TEST(SignatureInference, ResultAliasesArgument) {
  const auto result = analyze(std::string(Types) + R"c(
    char *strchr(const char *s, int c);
    static struct node *next_of(struct node *n) { return n->next; }
    static int *field_of(struct node *n) { return &n->v; }
    char *f(char *OWNED p) {
      char *q = strchr(p, 'x');
      free(p);
      return q;
    }
    void g(struct node *n) {
      struct node *m = next_of(n);
      free(n->next);
      use(m);
    }
    void h(void) {
      int *v;
      {
        struct node n;
        v = field_of(&n);
      }
      use(v);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: use of 'q' after it was freed",
                     "13: use of 'm' after it was freed",
                     "19: 'v' may outlive 'n', which it points to"}));
}

TEST(SignatureInference, OwnershipPassesThroughAConsumingCallee) {
  const auto result = analyze(R"c(
    static char *through(char *OWNED p) { return p; }
    static char *fill(char *OWNED p) { p[0] = 0; return p; }
    void f(void) {
      char *p = malloc(8);
      char *q = through(p);
      q[0] = 1;
      free(q);
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"9: use of 'p' after it was moved"}));
}

TEST(SignatureInference, GlobalsFreedByCallee) {
  const auto result = analyze(R"c(
    static char *cache;
    static void cache_free(void) { free(cache); }
    static void cache_reset(void) { free(cache); cache = NULL; }
    void f(void) {
      cache = malloc(8);
      cache_free();
      cache[0] = 1;
    }
    void g(void) {
      cache = malloc(8);
      cache_reset();
      cache = malloc(8);
      free(cache);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: use of 'cache' after it was freed"}));
}

TEST(SignatureInference, ParameterReassignedInCallee) {
  const auto result = analyze(std::string(Types) + R"c(
    static void free_and_null(char *p) { free(p); p = NULL; }
    static void list_free(struct node *head) {
      while (head) { struct node *next = head->next; free(head); head = next; }
    }
    void f(void) {
      char *p = malloc(8);
      free_and_null(p);
      p[0] = 1;
    }
    void g(struct node *l) {
      list_free(l);
      use(l);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"9: use of 'p' after it was freed",
                     "13: use of 'l' after it was freed"}));
}

TEST(SignatureInference, ConditionalEffectsAreMayEffects) {
  // RFC 0003: freed on one path is freed, unless the caller tests the
  // result that tells the paths apart (RFC 0006, *Outcome-conditional
  // summaries*): `f` is clean, `untested` is not.
  const auto result = analyze(R"c(
    static int free_if(char *p, int c) { if (c) { free(p); return 1; } return 0; }
    static struct s *pick(struct s *a, struct s *b, int c) { return c ? a : b; }
    void f(void) {
      char *p = malloc(8); if (!p) return;
      if (free_if(p, cond())) return;
      p[0] = 1;
    }
    void untested(void) {
      char *p = malloc(8); if (!p) return;
      free_if(p, cond());
      p[0] = 1;
    }
    void g(struct s *a, struct s *b) {
      struct s *p = pick(a, b, cond());
      free(a);
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // `f` is clean of use-after-free; on the path where `free_if` declined,
  // nobody releases `p` (RFC 0007).
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: 'p' is leaked", "12: use of 'p' after it was freed",
                     "17: use of 'p' after it was freed"}));
  const core::FunctionSummary *summary = result.summary("free_if");
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(summary->frees(0));
  EXPECT_FALSE(summary->consumesUnconditionally(core::SummaryPath::param(0)));
  EXPECT_TRUE(summary->outcomes.at(core::Outcome::Positive)
                  .at(core::SummaryPath::param(0))
                  .freed);
  EXPECT_FALSE(summary->outcomes.at(core::Outcome::Zero)
                   .contains(core::SummaryPath::param(0)));
  EXPECT_FALSE(summary->outcomes.contains(core::Outcome::Negative));
}

TEST(SignatureInference, BorrowForTheCall) {
  // Exclusivity between borrows is opt-in (RFC 0006, *Conflict rules*).
  const std::string code = std::string(Types) + R"c(
    static int get(const struct node *n) { return n->v; }
    static void set(struct node *n) { n->v = 1; }
    void f(void) {
      struct node x;
      const struct node *r = &x;
      get(&x);          /* shared while shared: fine */
      set(&x);          /* mutable while shared: conflict when exclusive */
      use(r);
    }
    void g(struct node *n) {
      struct node *m = n;
      get(m);           /* passing the holder, not a new borrow: fine */
      set(n);
    }
  )c";
  const auto lenient = analyze(code);
  ASSERT_TRUE(lenient.ast);
  EXPECT_TRUE(lenient.diagnostics.empty());

  const auto exclusive = analyze(code, {.exclusiveBorrows = true});
  ASSERT_TRUE(exclusive.ast);
  EXPECT_EQ(messages(exclusive.diagnostics),
            (Strings{"8: cannot borrow 'x' as mutable because it is already "
                     "borrowed"}));
}

TEST(SignatureInference, UnresolvableArgumentsAreDropped) {
  const auto result = analyze(std::string(Types) + R"c(
    static void node_free(struct node *n) { free(n); }
    static struct node *node_new(void) { return malloc(sizeof(struct node)); }
    void f(struct node *(*make)(void), void (*drop)(struct node *)) {
      node_free(NULL);
      node_free(node_new());
      free(malloc(1));
      struct node *n = make();   /* indirect, unresolvable: unknown */
      drop(n);                   /* indirect, unresolvable: no effect */
      use(n);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // The two unresolvable indirect calls are checking boundaries and warn
  // once per function type (RFC 0004, *Boundaries*); nothing is an error.
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"annotation-required", "annotation-required"}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: call through 'make' is not checked: its function type "
                     "has no ownership annotations and no function of that "
                     "type has its address taken in this program",
                     "9: call through 'drop' is not checked: its function "
                     "type has no ownership annotations and no function of "
                     "that type has its address taken in this program"}));
}

TEST(SignatureInference, CopiedLoansAreLifetimeChecked) {
  // Bug fix against RFC 0001 landed with RFC 0003: a plain pointer copy is
  // checked like a fresh borrow.
  const auto result = analyze(R"c(
    static int *g;
    void f(void) {
      int x = 0;
      int *p = &x;
      g = p;
    }
    void ok(void) {
      int x = 0;
      int *p = &x;
      int *q = p;
      use(q);
    }
    int *bad(void) {
      int *q;
      {
        int y = 0;
        int *p = &y;
        q = p;
      }
      return q;         /* reported once, where it escaped */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: 'g' may outlive 'x', which it points to",
                     "19: 'q' may outlive 'y', which it points to"}));
  EXPECT_EQ(notes(result.diagnostics, 1),
            (Strings{"'y' is declared here", "'y' goes out of scope here"}));
}

// -- Reconciliation (RFC 0003, "Reconciliation") ------------------------------

TEST(SignatureInference, AnnotationMismatchOnParameters) {
  const auto result = analyze(std::string(Types) + R"c(
    void free_shared(struct node *BORROWED n) { free(n); }
    void free_alias(struct node *BORROWED n) { struct node *m = n; free(m); }
    void move_mut(struct node *MUT n) { take(n); }
    void write_shared(struct node *BORROWED n) { n->v = 1; }
    void free_field(struct buf *BORROWED b) { free(b->data); }
    void pass_to_mut(struct node *BORROWED n) { poke(n); }
    void fine(struct node *MUT n, struct buf *BORROWED b) { n->v = 1; use(b->data); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            Strings(6, std::string(core::diag::AnnotationMismatch)));
  // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): wrapped literals
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "2: 'n' is annotated WEAVEC_BORROWED but is freed here",
          "3: 'n' is annotated WEAVEC_BORROWED but is freed here",
          "4: 'n' is annotated WEAVEC_MUT but is moved here",
          "5: 'n' is annotated WEAVEC_BORROWED but is written through here",
          "6: 'b' is annotated WEAVEC_BORROWED but 'b->data' is freed here",
          "7: 'n' is annotated WEAVEC_BORROWED but is written through "
          "here"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"'n' is annotated here"}));
  EXPECT_EQ(notes(result.diagnostics, 1),
            (Strings{"'n' is annotated here", "'m' is a copy of 'n'"}));
  for (const core::Diagnostic &d : result.diagnostics.diagnostics())
    EXPECT_EQ(d.severity, core::Severity::Error);
}

TEST(SignatureInference, AnnotationMismatchOnReturn) {
  const auto result = analyze(std::string(Types) + R"c(
    char *OWNED ret_borrow(struct buf *b) { return (char *)&b->len; }
    char *BORROWED ret_fresh(void) { return malloc(4); }
    char *MUT ret_fresh_local(void) { char *p = malloc(4); return p; }
    char *OWNED ret_copy(struct buf *b) { return b->data; }
    char *OWNED ret_fresh_ok(void) { return malloc(4); }
    char *BORROWED ret_field_ok(struct buf *b) { return b->data; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"2: function returns a borrow but its return type is "
                     "annotated WEAVEC_OWNED",
                     "3: function returns a fresh allocation but its return "
                     "type is annotated WEAVEC_BORROWED",
                     "4: function returns a fresh allocation but its return "
                     "type is annotated WEAVEC_MUT"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"annotated here"}));
}

TEST(SignatureInference, CallersTrustTheAnnotation) {
  const auto result = analyze(std::string(Types) + R"c(
    static void lies(struct node *BORROWED n) { free(n); }
    void f(struct node *n) {
      lies(n);
      use(n);           /* the caller believes BORROWED: no report here */
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"2: 'n' is annotated WEAVEC_BORROWED but is freed here"}));
}

// -- annotation-required (RFC 0003, "annotation-required, revised") -----------

TEST(SignatureInference, UnknownExternalCalleeWarnsOnce) {
  const auto result = analyze(R"c(
    void mystery(void *p);
    int pure(int x);
    void *maker(void);
    void f(char *p) {
      mystery(p);
      mystery(p);
      pure(1);
      use(maker());
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            Strings(2, std::string(core::diag::AnnotationRequired)));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: call to 'mystery' is not checked: it has no "
                     "definition or ownership annotations here",
                     "9: call to 'maker' is not checked: it has no definition "
                     "or ownership annotations here"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"'mystery' is declared here",
                     "annotate its pointer parameters with WEAVEC_OWNED, "
                     "WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it "
                     "in this program"}));
  EXPECT_EQ(result.diagnostics.diagnostics()[0].notes[0].location.line, 2U);
  EXPECT_EQ(result.diagnostics.diagnostics()[0].severity,
            core::Severity::Warning);
}

TEST(SignatureInference, StrictExternsMakesUncheckedCallsRawOperations) {
  // RFC 0004, "Boundaries": under --strict-externs an unchecked call is a
  // raw operation, so it is an error outside and permitted inside an unsafe
  // region, and its result is a raw pointer.
  AnalysisOptions options;
  options.strictExterns = true;
  const auto result = analyze(R"c(
    void mystery(void *p);
    void *maker(void);
    void f(char *p) { mystery(p); mystery(p); }
    void g(void) {
      char *q;
      UNSAFE { q = maker(); }
      *q = 1;
    }
  )c",
                              options);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      ids(result.diagnostics),
      (Strings{"unsafe-operation", "unsafe-operation", "unsafe-operation"}));
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"4: unchecked call to 'mystery' outside an unsafe region",
               "4: unchecked call to 'mystery' outside an unsafe region",
               "8: dereference of raw pointer 'q' outside an unsafe region"}));
  EXPECT_EQ(result.diagnostics.diagnostics()[0].severity,
            core::Severity::Error);
  EXPECT_EQ(notes(result.diagnostics, 2),
            (Strings{"'q' is raw: returned by a call into unchecked code "
                     "('maker') here",
                     "move this operation into a WEAVEC_UNSAFE block or "
                     "function, or assert the pointer's ownership first"}));
}

TEST(SignatureInference, ReportUnannotatedOffersTheInferredAnnotation) {
  AnalysisOptions options;
  options.reportUnannotated = true;
  const auto result = analyze(std::string(Types) + R"c(
    void frees(struct node *n) { free(n); }
    void reads(const struct node *n, int k) { use(n); }
    void writes(struct node *n) { n->v = 1; }
    void *makes(void) { return malloc(4); }
    int *borrows(struct node *n) { return &n->v; }
    void untouched(int *p) {}
    static void helper(struct node *n) { free(n); }
    void done(struct node *OWNED n, int *MUT q) { free(n); *q = 1; }
    int main(int argc, char **argv) { return argc; }
  )c",
                              options);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            Strings(7, std::string(core::diag::AnnotationRequired)));
  // NOLINTNEXTLINE(bugprone-suspicious-missing-comma): wrapped literals
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"2: pointer parameter 'n' of 'frees' is inferred WEAVEC_OWNED; "
               "add the annotation to its declaration",
               "3: pointer parameter 'n' of 'reads' is inferred "
               "WEAVEC_BORROWED; add the annotation to its declaration",
               "4: pointer parameter 'n' of 'writes' is inferred WEAVEC_MUT; "
               "add the annotation to its declaration",
               "5: return value of 'makes' is inferred WEAVEC_OWNED; add the "
               "annotation to its declaration",
               "6: pointer parameter 'n' of 'borrows' is inferred "
               "WEAVEC_BORROWED; add the annotation to its declaration",
               "6: return value of 'borrows' is inferred WEAVEC_BORROWED; add "
               "the annotation to its declaration",
               "7: pointer parameter 'p' has no inferable ownership; annotate "
               "it with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT"}))
      << "static helpers, annotated positions and main are not reported";

  const core::Diagnostic &frees = result.diagnostics.diagnostics()[0];
  ASSERT_EQ(frees.fixits.size(), 1U);
  EXPECT_EQ(frees.fixits[0].insertion, "WEAVEC_OWNED ");
  EXPECT_EQ(frees.fixits[0].location, frees.location);
  EXPECT_TRUE(result.diagnostics.diagnostics()[6].fixits.empty())
      << "nothing to suggest when inference is empty";
}

// -- Driver
// ---------------------------------------------------------------------

TEST(SignatureInference, FilteredFunctionsStillContributeSummaries) {
  const auto result = weavec::test::analyze(std::string(Types) + R"c(
    static void node_free(struct node *n) { free(n); free(n); }
    void f(struct node *n) { node_free(n); use(n); }
  )c");
  ASSERT_TRUE(result.ast);
  // Re-run with a filter that hides the helper's own bug.
  core::DiagnosticCollector filtered;
  TranslationUnitAnalyzer analyzer(result.ast->getASTContext(), filtered);
  analyzer.run(
      [](const clang::FunctionDecl &fn) { return fn.getName() == "f"; });
  EXPECT_EQ(messages(filtered), (Strings{"3: use of 'n' after it was freed"}));
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"2: 'n' is freed twice", "3: use of 'n' after it was freed"}));
}

TEST(SignatureInference, DumpIncludesTheSummary) {
  std::string dump;
  llvm::raw_string_ostream stream(dump);
  AnalysisOptions options;
  options.dumpStream = &stream;
  const auto result = analyze(std::string(Types) + R"c(
    static void buf_destroy(struct buf *b) { free(b->data); b->data = NULL; }
    static char *g;
    static char *keep(char *p) { g = p; return p; }
  )c",
                              options);
  ASSERT_TRUE(result.ast);
  EXPECT_NE(dump.find("summary: b->data: written|freed(free)|replaced; "
                      "stores{b->data = null} returns{}"),
            std::string::npos)
      << dump;
  EXPECT_NE(dump.find("summary: stores{g = copy p} returns{copy p}"),
            std::string::npos)
      << dump;
}

} // namespace
} // namespace weavec::analysis
