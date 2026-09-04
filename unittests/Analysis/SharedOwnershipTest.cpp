//===- SharedOwnershipTest.cpp - Reference counts, per-outcome stores -----===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0010: shared ownership. Each test names the RFC section it pins.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"
#include "weavec/Core/Scalar.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using core::Outcome;
using core::SummaryPath;
using core::ValueFact;
using test::analyze;
using test::analyzeInProgram;
using test::ids;
using test::messages;
using test::notes;

using Strings = std::vector<std::string>;

/// A counted object and the three functions every reference-counted C
/// library has. Line numbers in the tests count from the `#line 1`.
constexpr const char *Counted = R"c(
struct obj { int rc; char *name; };
static struct obj *obj_new(void) {
  struct obj *o = malloc(sizeof *o);
  if (!o) return NULL;
  o->rc = 1;
  o->name = malloc(4);
  return o;
}
static struct obj *obj_ref(struct obj *o) { o->rc++; return o; }
static void obj_unref(struct obj *o) {
  if (--o->rc == 0) { free(o->name); free(o); }
}
#line 1
)c";

// -- Inference (RFC 0010, *Inferring reference counts*) ----------------------

TEST(SharedOwnership, RefAndUnrefAreInferred) {
  const auto result = analyze(std::string(Counted) + R"c(
    void touch(struct obj *o) { use(o); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});

  const SummaryPath rc = SummaryPath::param(0).deref().field("rc");
  const core::FunctionSummary *ref = result.summary("obj_ref");
  ASSERT_NE(ref, nullptr);
  EXPECT_TRUE(ref->increments.contains(rc));
  EXPECT_TRUE(ref->retains(0));
  EXPECT_TRUE(ref->decrements.empty());
  EXPECT_TRUE(ref->counts.empty());
  EXPECT_FALSE(ref->effectOf(SummaryPath::param(0)).consumed())
      << "an increment is not a consume";

  const core::FunctionSummary *unref = result.summary("obj_unref");
  ASSERT_NE(unref, nullptr);
  EXPECT_TRUE(unref->decrements.contains(rc));
  EXPECT_TRUE(unref->counts.contains(rc));
  const core::PlaceEffect effect = unref->effectOf(SummaryPath::param(0));
  EXPECT_TRUE(effect.freed);
  EXPECT_TRUE(effect.share) << "a zero-guarded free after a decrement";
  EXPECT_EQ(effect.family, "free");
  EXPECT_FALSE(
      unref->effectOf(SummaryPath::param(0).deref().field("name")).consumed())
      << "the object's own fields go with the share";
  EXPECT_TRUE(result.analyzer->summaries().isKnownCount("struct obj.rc"));
  EXPECT_TRUE(result.analyzer->exports().countFields.contains("struct obj.rc"));
}

TEST(SharedOwnership, OtherDecrementSpellingsAreReleases) {
  const auto result = analyze(R"c(
    struct obj { int rc; };
    int __atomic_fetch_sub_int(int *, int, int);
    void post(struct obj *o) { if (o->rc-- == 1) free(o); }
    void compound(struct obj *o) { o->rc -= 1; if (o->rc == 0) free(o); }
    void atomic(struct obj *o) {
      if (__atomic_fetch_sub(&o->rc, 1, 5) == 1) free(o);
    }
    void sync(struct obj *o) {
      if (__sync_sub_and_fetch(&o->rc, 1) == 0) free(o);
    }
    void plain_free(struct obj *o) { free(o); }
    void unguarded(struct obj *o) { o->rc--; free(o); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
  for (const char *name : {"post", "compound", "atomic", "sync"}) {
    const core::FunctionSummary *summary = result.summary(name);
    ASSERT_NE(summary, nullptr) << name;
    EXPECT_TRUE(summary->effectOf(SummaryPath::param(0)).share) << name;
    EXPECT_TRUE(
        summary->decrements.contains(SummaryPath::param(0).deref().field("rc")))
        << name;
  }
  // A free with no decrement, or one not guarded by the count reaching
  // zero, is a plain free (RFC 0010, *A release is not a share release*).
  EXPECT_FALSE(
      result.summary("plain_free")->effectOf(SummaryPath::param(0)).share);
  EXPECT_FALSE(
      result.summary("unguarded")->effectOf(SummaryPath::param(0)).share);
  EXPECT_TRUE(result.summary("unguarded")->counts.empty());
}

TEST(SharedOwnership, IncrementsCrossCalleesAndVoidRefDoesNotLeak) {
  const auto result = analyze(std::string(Counted) + R"c(
    void keep(struct obj *o) { obj_ref(o); }
    void keep_twice(struct obj *o) { keep(o); o->rc++; }
    void drop(struct obj *o) { obj_unref(o); }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{})
      << "a share taken on a parameter is the caller's, not a leak";
  const SummaryPath rc = SummaryPath::param(0).deref().field("rc");
  EXPECT_TRUE(result.summary("keep")->increments.contains(rc));
  EXPECT_TRUE(result.summary("keep_twice")->increments.contains(rc));
  const core::FunctionSummary *drop = result.summary("drop");
  EXPECT_TRUE(drop->effectOf(SummaryPath::param(0)).share)
      << "wrappers carry it";
  EXPECT_TRUE(drop->counts.contains(rc));
}

// -- Checking (RFC 0010, *Bugs caught* and *Correct code accepted*) ----------

TEST(SharedOwnership, SharesAreCountedOnTheHolder) {
  const auto result = analyze(std::string(Counted) + R"c(
    int balanced(void) {
      struct obj *a = obj_new();
      if (!a) return -1;
      struct obj *b = obj_ref(a);
      obj_unref(b);
      use(a->name);
      obj_unref(a);
      return 0;
    }
    int self_ref(void) {
      struct obj *a = obj_new();
      if (!a) return -1;
      obj_ref(a);
      obj_unref(a);
      use(a->name);
      obj_unref(a);
      return 0;
    }
    int one_too_many(void) {
      struct obj *a = obj_new();
      if (!a) return -1;
      obj_ref(a);
      obj_unref(a);
      obj_unref(a);
      obj_unref(a);
      return 0;
    }
    int use_after_last(void) {
      struct obj *a = obj_new();
      if (!a) return -1;
      obj_unref(a);
      return a->rc;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"double-free", "use-after-free"}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"26: 'a' is released twice",
                     "33: use of 'a' after its reference was released"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"previously released here"}));
  EXPECT_EQ(notes(result.diagnostics, 1), (Strings{"reference released here"}));
}

TEST(SharedOwnership, ASplitShareOutlivesItsSource) {
  const auto result = analyze(std::string(Counted) + R"c(
    struct holder { struct obj *o; };
    int stored_share(struct holder *h) {
      struct obj *a = obj_new();
      if (!a) return -1;
      h->o = obj_ref(a);
      obj_unref(a);
      return h->o->rc;
    }
    int released_share(struct holder *h) {
      struct obj *a = obj_new();
      if (!a) return -1;
      h->o = obj_ref(a);
      obj_unref(h->o);
      use(a->name);
      obj_unref(a);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{})
      << "a copy that takes a surplus share is its own share";
}

TEST(SharedOwnership, APlainFreeKillsEveryShare) {
  const auto result = analyze(std::string(Counted) + R"c(
    int direct(void) {
      struct obj *a = obj_new();
      if (!a) return -1;
      struct obj *b = obj_ref(a);
      free(a);
      return b->rc;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: use of 'b' after it was freed"}));
  EXPECT_EQ(notes(result.diagnostics, 0),
            (Strings{"freed here (through 'a')"}));
}

TEST(SharedOwnership, ReleasingAnUnownedShareIsADiscipline) {
  const auto result = analyze(std::string(Counted) + R"c(
    int borrowed(struct obj *o) {
      obj_unref(o);
      return o->rc;
    }
    int retained(struct obj *o) {
      obj_ref(o);
      obj_unref(o);
      return o->rc;
    }
    int copy_of_retained(struct obj *o) {
      obj_ref(o);
      struct obj *q = o;
      obj_unref(q);
      return q->rc;
    }
    int loop(struct obj *o, int n) {
      int total = 0;
      for (int i = 0; i < n; ++i) {
        struct obj *t = obj_ref(o);
        total += t->rc;
        obj_unref(t);
      }
      return total;
    }
  )c");
  ASSERT_TRUE(result.ast);
  // A share this function does not own is released: the name is dead after
  // it. A share it retained first is its own to release.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: use of 'o' after its reference was released"}));
}

TEST(SharedOwnership, LeaksOfShares) {
  const auto result = analyze(std::string(Counted) + R"c(
    void keep(struct obj *o) { obj_ref(o); }
    int caller_loses_the_share(void) {
      struct obj *a = obj_new();
      if (!a) return -1;
      keep(a);
      obj_unref(a);
      return 0;
    }
    struct list { struct obj *head; };
    void local_retained(struct list *l) {
      struct obj *p = l->head;
      obj_ref(p);
    }
    struct sized { int len; char *buf; };
    void not_a_count(struct sized *s) {
      struct sized *t = s;
      t->len++;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), (Strings{"leak", "leak"}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: 'a' is leaked", "13: 'p' is leaked"}));
  EXPECT_EQ(notes(result.diagnostics, 0), (Strings{"allocated here"}));
  EXPECT_EQ(notes(result.diagnostics, 1), (Strings{"reference taken here"}));
}

// -- Stores out of sight (RFC 0010, *Stores out of sight*) --------------------

/// A container that keeps its values in nodes of its own, and the wrappers
/// every reference-counted library puts around it.
constexpr const char *Table = R"c(
struct pair { struct pair *next; struct obj *value; };
struct table { struct pair *first; };
static int table_set(struct table *t, struct obj *value) {
  struct pair *p = malloc(sizeof *p);
  if (!p) return -1;
  p->value = value;
  p->next = t->first;
  t->first = p;
  return 0;
}
static int table_set_new(struct table *t, struct obj *value) {
  if (!value) return -1;
  if (table_set(t, value)) { obj_unref(value); return -1; }
  return 0;
}
static int table_set_shared(struct table *t, struct obj *value) {
  return table_set_new(t, obj_ref(value));
}
)c";

TEST(SharedOwnership, AStoreBelowANodeIsAnEscape) {
  const auto result = analyze(std::string(Counted) + Table + R"c(
    struct obj *g_value;
    static struct obj *iter_value(void *it) { return it ? g_value : NULL; }
    int owned_into_table(struct table *t) {
      struct obj *o = obj_new();
      if (!o) return -1;
      return table_set(t, o);
    }
    int share_into_table(struct table *t, void *it) {
      struct obj *value = iter_value(it);
      if (!value) return -1;
      return table_set_shared(t, value);
    }
    int shares_into_table(struct table *t, void *it) {
      struct obj *value;
      while ((value = iter_value(it)) != NULL) {
        if (table_set_shared(t, value)) return -1;
      }
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{})
      << "the node keeps the value, and the share the wrapper took";

  const SummaryPath value = SummaryPath::param(1);
  const core::FunctionSummary *set = result.summary("table_set");
  ASSERT_NE(set, nullptr);
  EXPECT_TRUE(set->effectOf(value).escaped);
  EXPECT_TRUE(
      set->effectOf(SummaryPath::param(0).deref().field("first")).escaped)
      << "the old head lives on in the new node";
  EXPECT_FALSE(set->effectOf(value).consumed());
  EXPECT_TRUE(set->stores.empty() ||
              !set->stores.begin()->dest.isProperPrefixOf(value))
      << "no destination the caller could name";
  // Wrappers pass it on, including through the returning-ref shape.
  EXPECT_TRUE(result.summary("table_set_new")->effectOf(value).escaped);
  const core::FunctionSummary *shared = result.summary("table_set_shared");
  ASSERT_NE(shared, nullptr);
  EXPECT_TRUE(shared->effectOf(value).escaped);
  EXPECT_TRUE(shared->increments.contains(value.deref().field("rc")));
  EXPECT_TRUE(shared->decrements.contains(value.deref().field("rc")))
      << "`table_set_new(t, obj_ref(value))` resolves `param 1` to `value`";
}

TEST(SharedOwnership, AStoreIntoALocalIsNotAnEscape) {
  const auto result = analyze(std::string(Counted) + R"c(
    struct ctx { struct obj *o; };
    int use_locally(struct obj *o) {
      struct ctx c;
      struct ctx *p = &c;
      p->o = o;
      return p->o->rc;
    }
    int caller(void) {
      struct obj *a = obj_new();
      if (!a) return -1;
      return use_locally(a);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_FALSE(
      result.summary("use_locally")->effectOf(SummaryPath::param(0)).escaped)
      << "a node on the stack dies with the call";
  EXPECT_EQ(messages(result.diagnostics), (Strings{"12: 'a' is leaked"}));
}

// -- Per-outcome stores (RFC 0010, *Per-outcome stores*) ----------------------

TEST(SharedOwnership, StoresAreRetractedOnOutcomesThatDoNotStore) {
  const auto result = analyze(R"c(
    struct bag { char *items[8]; int n; };
    int bag_put(struct bag *b, char *s) {
      if (b->n == 8) return -1;
      b->items[b->n++] = s;
      return 0;
    }
    int put_or_free(struct bag *b) {
      char *s = malloc(8);
      if (!s) return -1;
      if (bag_put(b, s) < 0) {
        free(s);
        return -1;
      }
      return 0;
    }
    int put_and_free(struct bag *b) {
      char *s = malloc(8);
      if (!s) return -1;
      if (bag_put(b, s) == 0) return 0;
      free(s);
      return -1;
    }
    int put_ignored(struct bag *b) {
      char *s = malloc(8);
      if (!s) return -1;
      bag_put(b, s);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});

  const core::FunctionSummary *put = result.summary("bag_put");
  ASSERT_NE(put, nullptr);
  const SummaryPath items =
      SummaryPath::param(0).deref().field("items").indexed();
  const SummaryPath n = SummaryPath::param(0).deref().field("n");
  EXPECT_EQ(put->storesOnClass(Outcome::Zero), (std::set<SummaryPath>{items}));
  EXPECT_TRUE(put->storesOnClass(Outcome::Negative).empty());
  EXPECT_TRUE(put->effectOf(n).written);
  EXPECT_FALSE(put->factOn.contains(Outcome::Zero))
      << "`b->n` was incremented from a value known only not to be 8";
  ASSERT_TRUE(put->factOn.contains(Outcome::Negative));
  EXPECT_EQ(put->factOn.at(Outcome::Negative).at(n), ValueFact::ofConstant(8))
      << "the test that failed the call holds at its return";
}

TEST(SharedOwnership, ARetractedStoreLeavesTheResourceWithTheCaller) {
  const auto result = analyze(R"c(
    int put(char **slot, char *s, int room) {
      if (!room) return -1;
      *slot = s;
      return 0;
    }
    void retracted(char **slot, int room) {
      char *s = malloc(8);
      if (put(slot, s, room) < 0) {
        return;
      }
    }
    void kept(char **slot, int room) {
      char *s = malloc(8);
      if (put(slot, s, room) == 0)
        return;
      use(s);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // `retracted`: on the failure edge the store did not happen, so `s` is
  // still this function's and is lost at the return. `kept`: on the failure
  // edge it is lost at the end.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"10: 's' is leaked", "17: 's' is leaked"}));
}

TEST(SharedOwnership, PerOutcomeFactsFlowIntoTheCaller) {
  const auto result = analyze(R"c(
    struct box { int filled; char *p; };
    int fill(struct box *b, char *p) {
      if (!p) { b->filled = 0; return -1; }
      b->p = p;
      b->filled = 1;
      return 0;
    }
    void consumer(struct box *b) {
      char *p = malloc(8);
      if (fill(b, p) < 0) {
        if (b->filled) use(b->p);
        free(p);
      }
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
  const core::FunctionSummary *fill = result.summary("fill");
  ASSERT_NE(fill, nullptr);
  const SummaryPath filled = SummaryPath::param(0).deref().field("filled");
  EXPECT_EQ(fill->factOn.at(Outcome::Negative).at(filled),
            ValueFact::ofConstant(0));
  EXPECT_EQ(fill->factOn.at(Outcome::Zero).at(filled),
            ValueFact::ofConstant(1));
}

// -- Annotations (RFC 0010, *Annotations*) -----------------------------------

TEST(SharedOwnership, AnnotatedRetainAndRelease) {
  const auto result = analyze(R"c(
    struct obj;
    struct obj *g_new(void) OWNED;
    struct obj *g_ref(struct obj *RETAINS o);
    void g_unref(struct obj *RELEASES o);
    int balanced(void) {
      struct obj *a = g_new();
      if (!a) return -1;
      struct obj *b = g_ref(a);
      g_unref(b);
      g_unref(a);
      return 0;
    }
    int twice(void) {
      struct obj *a = g_new();
      if (!a) return -1;
      g_unref(a);
      g_unref(a);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"18: 'a' is released twice"}))
      << "the result of a returning ref is a copy of its argument";
}

TEST(SharedOwnership, OwnedByNamesTheFamily) {
  const auto result = analyze(R"c(
    struct obj;
    void obj_destroy(struct obj *OWNED o);
    struct obj *obj_make(void) OWNED OWNED_BY(obj_destroy);
    int right(void) {
      struct obj *o = obj_make();
      if (!o) return -1;
      obj_destroy(o);
      return 0;
    }
    int wrong(void) {
      struct obj *o = obj_make();
      if (!o) return -1;
      free(o);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), (Strings{"mismatched-release"}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"14: 'o' is released with 'free' but must be released "
                     "with 'obj_destroy'"}));
}

TEST(SharedOwnership, InvalidShareAnnotations) {
  const auto result = analyze(R"c(
    struct obj;
    void both(struct obj *RETAINS RELEASES o) { use(o); }
    struct obj *family_alone(void) OWNED_BY(obj_destroy) { return NULL; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"invalid-annotation", "invalid-annotation"}));
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"3: 'o' is declared both WEAVEC_RETAINS and WEAVEC_RELEASES",
               "4: 'family_alone' is declared WEAVEC_OWNED_BY(obj_destroy) "
               "without WEAVEC_OWNED"}));
}

// -- Whole program (RFC 0010, *Across translation units*) ---------------------

TEST(SharedOwnership, CountFieldsAndShareReleasesCrossUnits) {
  const auto library = analyze(std::string(Counted) + R"c(
    struct obj *make(void) { return obj_new(); }
    struct obj *retain(struct obj *o) { return obj_ref(o); }
    void release(struct obj *o) { obj_unref(o); }
  )c");
  ASSERT_TRUE(library.ast);
  ProgramDatabase db;
  db.add(library.analyzer->exports());
  EXPECT_TRUE(db.isKnownCount("struct obj.rc"));
  ASSERT_NE(db.find("release"), nullptr);
  EXPECT_TRUE(db.find("release")->effectOf(SummaryPath::param(0)).share);
  EXPECT_TRUE(db.find("retain")->retains(0));

  const auto client = analyzeInProgram(R"c(
    struct obj { int rc; char *name; };
    struct obj *make(void);
    struct obj *retain(struct obj *o);
    void release(struct obj *o);
    int fine(void) {
      struct obj *a = make();
      if (!a) return -1;
      struct obj *b = retain(a);
      release(b);
      release(a);
      return 0;
    }
    int bad(void) {
      struct obj *a = make();
      if (!a) return -1;
      release(a);
      release(a);
      return 0;
    }
    void lost(struct obj *o) {
      struct obj *p = o;
      retain(p);
    }
  )c",
                                       &db);
  ASSERT_TRUE(client.ast);
  EXPECT_EQ(messages(client.diagnostics),
            (Strings{"18: 'a' is released twice", "23: 'p' is leaked"}));
}

} // namespace
} // namespace weavec::analysis
