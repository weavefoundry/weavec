//===- SpatialSafetyTest.cpp - Tests for RFC 0011
//--------------------------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Derived pointers, deferred lifetimes, extents and bounds checks: the rules
// of RFC 0011 (*Spatial safety*), checked against the diagnostics and the
// summaries the checker produces on small programs.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"

#include <gtest/gtest.h>

namespace weavec::analysis {
namespace {

using core::PathAffine;
using core::PointerOffset;
using core::SummaryPath;
using core::ValueSource;
using weavec::test::analyze;
using weavec::test::ids;
using weavec::test::messages;
using weavec::test::notes;

using Strings = std::vector<std::string>;

constexpr const char *Types = R"c(
#define offsetof(T, f) __builtin_offsetof(T, f)
#define container_of(p, T, f) ((T *)((char *)(p) - offsetof(T, f)))
#define SIZED_BY(n) __attribute__((annotate("weavec.sized_by." #n)))
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
char *fgets(char *, int, void *);
struct inner { char *buf; };
struct outer { struct inner in; int k; };
struct link { struct link *next; };
struct list { struct link *head; };
struct node { struct link link; int v; };
#line 0
)c";

// -- Derived pointers (RFC 0011, *Derived pointers*) --------------------------

// A pointer to a field is a copy of its base at that field: a release
// through it reaches the summary as an effect on the base, at the offset.
TEST(DerivedPointers, ReleaseThroughAFieldPointerReachesTheSummary) {
  const auto result = analyze(std::string(Types) + R"c(
    static void release_inner(struct outer *o) {
      struct inner *i = &o->in;
      free(i->buf);
    }
    void caller(struct outer *o) {
      release_inner(o);
      use(o->in.buf);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"7: use of 'o->in.buf' after it was freed"}));
  const core::FunctionSummary *release = result.summary("release_inner");
  ASSERT_NE(release, nullptr);
  EXPECT_TRUE(
      release->effectOf(SummaryPath::param(0).deref().field("in").field("buf"))
          .freed);
}

// `container_of` walks back to the allocation: freeing the container of a
// member pointer frees the object the member pointer was derived from, so
// the round trip is clean and the summary records the offset of the free.
TEST(DerivedPointers, ContainerOfRoundTripsToTheAllocation) {
  const auto result = analyze(std::string(Types) + R"c(
    void free_container(struct inner *i) {
      free(container_of(i, struct outer, in));
    }
    void use_container(void) {
      struct outer *o = malloc(sizeof *o);
      if (!o) return;
      free_container(&o->in);
    }
    struct inner *make_inner(void) {
      struct outer *o = malloc(sizeof *o);
      if (!o) return 0;
      return &o->in;
    }
    void link_node(struct list *l) {
      struct node *n = malloc(sizeof *n);
      if (!n) return;
      n->link.next = l->head;
      l->head = &n->link;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
  const core::FunctionSummary *fc = result.summary("free_container");
  ASSERT_NE(fc, nullptr);
  const core::PlaceEffect effect = fc->effectOf(SummaryPath::param(0));
  EXPECT_TRUE(effect.freed);
  EXPECT_EQ(effect.at, PointerOffset::ofField("struct outer.in").negated())
      << "freed at minus the offset of `in`";
  // The member pointer handed out is the fresh object at the field.
  const core::FunctionSummary *make = result.summary("make_inner");
  ASSERT_NE(make, nullptr);
  const auto fresh = std::ranges::find_if(
      make->returns, [](const ValueSource &s) { return s.isFresh(); });
  ASSERT_NE(fresh, make->returns.end());
  EXPECT_EQ(fresh->offset, PointerOffset::ofField("struct outer.in"));
  EXPECT_EQ(fresh->extent, PathAffine::ofConstant(16));
}

// An array member decaying (`w->payload`) or its first element addressed
// (`&w->payload[0]`) is the base pointer stepped to the field, like `&w->tag`:
// a `container_of` release through either reaches the container, and an
// access through the pointer is bounded by the container's extent.
TEST(DerivedPointers, ArrayMembersDeriveFromTheirContainer) {
  const auto result = analyze(std::string(Types) + R"c(
    struct wrapped { int tag; char payload[8]; };
    static void wrapped_release(char *payload) {
      free(container_of(payload, struct wrapped, payload));
    }
    char *payload_of(struct wrapped *w) { return w->payload; }
    char *first_of(struct wrapped *w) { return &w->payload[0]; }
    char *third_of(struct wrapped *w) { return &w->payload[2]; }
    void release_decayed(void) {
      struct wrapped *w = malloc(sizeof *w);
      if (!w) return;
      wrapped_release(w->payload);
    }
    void release_addressed(void) {
      struct wrapped *w = malloc(sizeof *w);
      if (!w) return;
      wrapped_release(&w->payload[0]);
    }
    void release_twice(void) {
      struct wrapped *w = malloc(sizeof *w);
      if (!w) return;
      wrapped_release(w->payload);
      wrapped_release(w->payload);
    }
    void through_payload(void) {
      struct wrapped *w = malloc(sizeof *w);
      if (!w) return;
      char *p = w->payload;
      p[7] = 0;
      w->payload[8] = 0;
      free(w);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"22: 'w' is freed twice",
                     "29: 'w->payload[8]' is out of bounds: index 8 of an "
                     "object of 8 bytes"}));
  EXPECT_EQ(ids(result.diagnostics), (Strings{"double-free", "out-of-bounds"}));
  const auto returnedOffset =
      [&result](const char *name) -> std::optional<PointerOffset> {
    const core::FunctionSummary *s = result.summary(name);
    if (s == nullptr || s->returns.size() != 1)
      return std::nullopt;
    return s->returns.begin()->offset;
  };
  const PointerOffset payload =
      PointerOffset::ofField("struct wrapped.payload");
  EXPECT_EQ(returnedOffset("payload_of"), payload);
  EXPECT_EQ(returnedOffset("first_of"), payload);
  EXPECT_EQ(returnedOffset("third_of"), PointerOffset::inside())
      << "an element step below a field is somewhere inside (RFC 0011)";
}

// Releasing a derived pointer that is not at the start is invalid, and the
// message says where it points.
TEST(DerivedPointers, ReleaseAwayFromTheStartNamesTheOffset) {
  const auto result = analyze(std::string(Types) + R"c(
    void field_release(void) {
      struct outer *o = malloc(sizeof *o);
      if (!o) return;
      free(&o->in);
    }
    void bad_rebase(char *OWNED s) {
      char *p = s + 3;
      free(p - 2);
    }
    void step_up(char *OWNED s) {
      s++;
      free(s);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"4: 'o' is released but points to field 'in' of its allocation",
               "8: 'p' is released but points 1 element past the start of its "
               "allocation",
               "12: 's' is released but points 1 element past the start of "
               "its allocation"}));
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"invalid-release", "invalid-release", "invalid-release"}));
}

// Arithmetic that comes back to the start is a release of the allocation;
// `!=` against the start separates the walking pointer.
TEST(DerivedPointers, RebasingAndWalkingAreClean) {
  const auto result = analyze(std::string(Types) + R"c(
    void rebase(char *OWNED s) {
      char *p = s + 3;
      free(p - 3);
    }
    void walk(char *OWNED s) {
      char *p = s;
      while (*p) p++;
      free(s);
    }
    void walk_back(char *OWNED s, int n) {
      char *p = s;
      for (int i = 0; i < n; i++) p++;
      for (int i = 0; i < n; i++) p--;
      use(p);
      free(s);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
}

// A borrow of a field is a borrow of the object: freeing the object while a
// field pointer is still used is a use after free through the field pointer
// (RFC 0011 replaces the `conflicting-borrow` here).
TEST(DerivedPointers, FieldPointerOutlivingTheObjectIsAUseAfterFree) {
  const auto result = analyze(std::string(Types) + R"c(
    void through_field(void) {
      struct outer *o = malloc(sizeof *o);
      if (!o) return;
      struct inner *i = &o->in;
      free(o);
      use(i);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: use of 'i' after it was freed"}));
  EXPECT_EQ(ids(result.diagnostics), Strings{"use-after-free"});
}

// Freeing an object conflicts with borrows of its storage and of what it
// owns, not of objects the pointers stored in it merely refer to (RFC 0011,
// *Derived pointers*): jansson's bucket array holds pointers into pairs the
// intrusive list owns, and rehashing frees the array while the pairs are
// still borrowed through it.
TEST(DerivedPointers, FreeingAnObjectLeavesBorrowsOfWhatItPointsAtIntact) {
  const auto result = analyze(R"c(
    typedef struct list { struct list *prev; struct list *next; } list_t;
    typedef struct pair { list_t list; int v; } pair_t;
    typedef struct bucket { list_t *first; list_t *last; } bucket_t;
    typedef struct ht { bucket_t *buckets; list_t list; } ht_t;
    void *malloc(unsigned long);
    void free(void *);
    int rehash(ht_t *h, pair_t *pair) {
      bucket_t *nb = malloc(sizeof(bucket_t));
      if (!nb) return -1;
      h->buckets->last = &pair->list;
      pair->list.next = &h->list;
      free(h->buckets);
      h->buckets = nb;
      return 0;
    }
    // The same when the pair is owned, through the list, by the table: the
    // bucket's pointer shares the resource but the array does not own it.
    int rehash_owned(ht_t *h) {
      bucket_t *nb = malloc(sizeof(bucket_t));
      if (!nb) return -1;
      pair_t *pair = malloc(sizeof *pair);
      if (!pair) { free(nb); return -1; }
      h->list.next = &pair->list;
      h->buckets->last = h->list.next;
      h->buckets->first = &pair->list;
      free(h->buckets);
      h->buckets = nb;
      return 0;
    }
    // The object's own storage is another matter.
    struct node { int *buf; int n; };
    void own(struct node *node) {
      int *keep = &node->n;
      free(node);
      *keep = 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"36: use of 'keep' after it was freed"}));
}

// A pointer into caller memory reached through a local (`f = fs->f; f->ups =
// grow(...); return &f->ups[n]`) is a copy of the caller's place, not a fresh
// allocation handed out a second time (Lua's `allocupvalue`: `up` is not
// leaked in `newupvalue`).
TEST(DerivedPointers, AReturnedFieldOfALocalAliasNamesTheCallersPlace) {
  const auto result = analyze(R"c(
    void *realloc(void *, unsigned long);
    void abort(void);
    typedef struct Up { const char *name; int idx; } Up;
    typedef struct Proto { Up *ups; int size; } Proto;
    typedef struct FS { Proto *f; int nups; } FS;
    static Up *alloc_up(FS *fs) {
      Proto *f = fs->f;
      if (fs->nups + 1 > f->size) {
        Up *nb = realloc(f->ups, sizeof(Up) * (unsigned long)(f->size * 2 + 4));
        if (nb == 0) abort();
        f->ups = nb;
        f->size = f->size * 2 + 4;
      }
      return &f->ups[fs->nups++];
    }
    int new_up(FS *fs, const char *name) {
      Up *up = alloc_up(fs);
      up->name = name;
      return fs->nups - 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});
  const core::FunctionSummary *alloc = result.summary("alloc_up");
  ASSERT_NE(alloc, nullptr);
  ASSERT_EQ(alloc->returns.size(), 1U);
  const ValueSource &returned = *alloc->returns.begin();
  EXPECT_EQ(returned.kind, ValueSource::Kind::Copy);
  ASSERT_TRUE(returned.path.has_value());
  EXPECT_EQ(*returned.path,
            SummaryPath::param(0).deref().field("f").deref().field("ups"));
}

// Two pointers derived from one base at one offset are exact aliases; after
// a join the relation is a may-relation, so a null test of one says nothing
// definite about the other (RFC 0011, *Derived pointers and nullness*).
// Lua's `luaV_execute`: `ci` and `newci` both point into `L` on some
// iteration, and `newci == NULL` must not make `ci` null.
TEST(DerivedPointers, NullTestsDoNotTravelAlongMayAliases) {
  const auto result = analyze(R"c(
    typedef struct CI { int x; int trap; struct CI *next; } CI;
    typedef struct S { CI *ci; CI base_ci; } S;
    CI *pre(S *L, int c) {
      if (c) return 0;
      return &L->base_ci;
    }
    int exec(S *L, CI *ci, int c) {
      int v;
     start:
      v = ci->x;
      {
        CI *newci;
        if ((newci = pre(L, c)) == 0) v = ci->trap;
        else { ci = newci; goto start; }
      }
      return v;
    }
    // A copy made on this very path does hold the same null.
    int copy_on_path(S *L, int c) {
      CI *p = pre(L, c);
      CI *q = p;
      if (p == 0) return q->x;
      return 1;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"23: dereference of 'q', which is null"}));
}

// A local that equals a caller's place outright is named by it, not by a
// derived name it also carries (RFC 0011, *Summaries*): Lua's `ci = L->ci =
// next_ci(L)` is `copy L->ci`, not a copy into `L` at some offset.
TEST(DerivedPointers, ExactAliasesNameAReturnedLocal) {
  const auto result = analyze(R"c(
    typedef struct CI { int x; struct CI *next; } CI;
    typedef struct S { CI *ci; CI base_ci; } S;
    CI *step(S *L, int c) {
      if (c) L->ci = &L->base_ci;
      CI *ci = L->ci = L->ci->next;
      return ci;
    }
  )c");
  ASSERT_TRUE(result.ast);
  const core::FunctionSummary *step = result.summary("step");
  ASSERT_NE(step, nullptr);
  ASSERT_EQ(step->returns.size(), 1U);
  const ValueSource &returned = *step->returns.begin();
  EXPECT_EQ(returned.kind, ValueSource::Kind::Copy);
  ASSERT_TRUE(returned.path.has_value());
  EXPECT_EQ(*returned.path, SummaryPath::param(0).deref().field("ci"));
  EXPECT_TRUE(returned.offset.isZero());
}

// -- Deferred lifetimes (RFC 0011, *Deferred lifetime checks*) ----------------

// Storing the address of a local through a parameter is fine when the store
// is undone before the local dies; the report comes at the local's death.
TEST(DeferredLifetimes, ReportedWhenTheLocalDies) {
  const auto result = analyze(std::string(Types) + R"c(
    struct frame { struct frame *prev; int depth; };
    struct state { struct frame *fs; };
    void enter(struct state *st) {
      struct frame f;
      f.prev = st->fs;
      st->fs = &f;
      st->fs = f.prev;
    }
    void enter_bad(struct state *st) {
      struct frame f;
      f.prev = st->fs;
      st->fs = &f;
    }
    void enter_reset(struct state *st) {
      struct frame f;
      st->fs = &f;
      st->fs = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"12: 'st->fs' may outlive 'f', which it points to"}));
  EXPECT_EQ(ids(result.diagnostics), Strings{"lifetime-too-short"});
  EXPECT_EQ(notes(result.diagnostics),
            (Strings{"'f' is declared here", "'f' goes out of scope here"}));
}

// A local's address escaping through a return, a global, or into a
// longer-lived object stored on one path only is still reported.
TEST(DeferredLifetimes, EscapesOnAnyPathAreReported) {
  const auto result = analyze(std::string(Types) + R"c(
    struct state { int *p; };
    int *g;
    void one_path(struct state *st) {
      int local = 1;
      if (cond()) st->p = &local;
    }
    void to_global(void) {
      int local = 1;
      g = &local;
      g = 0;
      g = &local;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"5: 'st->p' may outlive 'local', which it points to",
                     "11: 'g' may outlive 'local', which it points to"}));
}

// -- Extents (RFC 0011, *Extents*) --------------------------------------------

TEST(Extents, AllocationsCarryTheirExtent) {
  const auto result = analyze(std::string(Types) + R"c(
    void *calloc(size_t, size_t);
    static char *sixteen(void) { return malloc(16); }
    static int *ints(int n) { return malloc(n * sizeof(int)); }
    static int *zeroed(int n) { return calloc(n, sizeof(int)); }
    static struct outer *one(void) { return malloc(sizeof(struct outer)); }
    static char *grown(char *OWNED p, size_t n) { return realloc(p, n + 1); }
  )c");
  ASSERT_TRUE(result.ast);
  const auto extentOf = [&result](const char *name) {
    const core::FunctionSummary *s = result.summary(name);
    if (s == nullptr)
      return std::optional<PathAffine>{};
    for (const ValueSource &source : s->returns)
      if (source.isFresh())
        return source.extent;
    return std::optional<PathAffine>{};
  };
  EXPECT_EQ(extentOf("sixteen"), PathAffine::ofConstant(16));
  EXPECT_EQ(extentOf("ints"), PathAffine::ofPath(SummaryPath::param(0), 4, 0));
  EXPECT_EQ(extentOf("zeroed"),
            PathAffine::ofPath(SummaryPath::param(0), 4, 0));
  EXPECT_EQ(extentOf("one"), PathAffine::ofConstant(16));
  EXPECT_EQ(extentOf("grown"), PathAffine::ofPath(SummaryPath::param(1), 1, 1));
}

// -- Bounds checks (RFC 0011, *Bounds checks*) --------------------------------

TEST(Bounds, ConstantIndexAgainstAConstantExtent) {
  const auto result = analyze(std::string(Types) + R"c(
    void arrays(void) {
      char buf[10];
      buf[9] = 0;
      buf[10] = 0;
      int a[4];
      a[3] = 0;
      a[4] = 0;
    }
    void heap(void) {
      char *p = malloc(8);
      if (!p) return;
      p[7] = 0;
      p[8] = 0;
      free(p);
    }
    void folded(void) {
      int data = 10;
      int buffer[10];
      buffer[data] = 1;
    }
    void deref(void) {
      int *q = malloc(2 * sizeof *q);
      if (!q) return;
      *(q + 1) = 1;
      *(q + 2) = 1;
      free(q);
    }
    void field(void) {
      struct outer *o = malloc(sizeof(struct inner));
      if (!o) return;
      o->k = 1;
      free(o);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "4: 'buf[10]' is out of bounds: index 10 of an object of 10 bytes",
          "7: 'a[4]' is out of bounds: index 4 of an object of 16 bytes",
          "13: 'p[8]' is out of bounds: index 8 of an object of 8 bytes",
          "19: 'buffer[data]' is out of bounds: index 'data' (10) of an "
          "object of 40 bytes",
          "25: '*(q + 2)' is out of bounds: index 2 of an object of 8 "
          "bytes",
          "31: 'o->k' is out of bounds: it reaches 12 bytes into 'o', "
          "which has 8 bytes"}));
  for (const std::string &id : ids(result.diagnostics))
    EXPECT_EQ(id, "out-of-bounds");
  EXPECT_EQ(notes(result.diagnostics, 0), Strings{"'buf' is declared here"});
  EXPECT_EQ(notes(result.diagnostics, 2), Strings{"'p' is allocated here"});
}

TEST(Bounds, BeforeTheStart) {
  const auto result = analyze(std::string(Types) + R"c(
    void negative(void) {
      char buf[16];
      char *p = buf;
      p[-1] = 0;
    }
    void underwrite(void) {
      char *buf = malloc(16);
      if (!buf) return;
      char *p = buf - 8;
      p[0] = 'A';
      free(buf);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: 'p[-1]' is out of bounds: index -1 is before the "
                     "start of 'p'",
                     "10: 'p[0]' is out of bounds: index 0 is before the start "
                     "of 'p'"}));
}

// The pointer's own position counts: `p + 2` then `[6]` on 8 elements.
TEST(Bounds, TheOffsetOfADerivedPointerCounts) {
  const auto result = analyze(std::string(Types) + R"c(
    void stepped(void) {
      int *q = malloc(8 * sizeof *q);
      if (!q) return;
      int *p = q + 2;
      p[5] = 0;
      p[6] = 0;
      free(q);
    }
    void walked(void) {
      char *s = malloc(4);
      if (!s) return;
      char *p = s;
      p++; p++; p++;
      *p = 0;
      p++;
      *p = 0;
      free(s);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"6: 'p[6]' is out of bounds: index 6 of an object of 32 "
                     "bytes",
                     "16: '*p' is out of bounds: it reaches 5 bytes into 'p', "
                     "which has 4 bytes"}));
}

TEST(Bounds, SymbolicIndexAgainstTheSameCounter) {
  const auto result = analyze(std::string(Types) + R"c(
    void at_n(int n) {
      int *p = malloc(n * sizeof *p);
      if (!p) return;
      p[n - 1] = 0;
      p[n] = 0;
      free(p);
    }
    void bytes(size_t n) {
      char *p = malloc(n + 1);
      if (!p) return;
      p[n] = 0;
      p[n + 1] = 0;
      free(p);
    }
    void copy_n(size_t n) {
      char *p = malloc(n);
      if (!p) return;
      memset(p, 0, n);
      memset(p, 0, n + 1);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "5: 'p[n]' is out of bounds: 'n' is the number of elements of 'p'",
          "12: 'p[n + 1]' is out of bounds: it reaches 'n' + 2 bytes into "
          "'p', which has 'n' + 1 bytes",
          "19: 'memset' accesses 'n' + 1 bytes of 'p', which has 'n' "
          "bytes"}));
}

// Relations learnt on edges decide symbolic accesses: `i < n` is in bounds,
// `i <= n` may be past the end, `i >= n` is past the end.
TEST(Bounds, RelationsFromConditions) {
  const auto result = analyze(std::string(Types) + R"c(
    void loops(int n) {
      int *p = malloc(n * sizeof *p);
      if (!p) return;
      for (int i = 0; i < n; i++) p[i] = 0;
      for (int i = 0; i <= n; i++) p[i] = 0;
      for (int i = 0; i < n; i++) p[i + 1] = 0;
      for (int i = 0; i < n - 1; i++) p[i + 1] = 0;
      free(p);
    }
    void guards(int i, int n) {
      int *p = malloc(n * sizeof *p);
      if (!p) return;
      if (i < n) p[i] = 1;
      if (i >= n) p[i] = 1;
      if (i > n) p[i] = 1;
      free(p);
    }
    void reversed(int i, int n) {
      int *p = malloc(n * sizeof *p);
      if (!p) return;
      if (n > i) p[i] = 1;
      if (n <= i) p[i] = 1;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"5: 'p[i]' may be out of bounds: 'i' may equal 'n', the number "
               "of elements of 'p'",
               "6: 'p[i + 1]' may be out of bounds: 'i' may reach one below "
               "'n', and 'p' has 'n' * 4 bytes",
               "14: 'p[i]' is out of bounds: 'i' is at least 'n', the number "
               "of elements of 'p'",
               "15: 'p[i]' is out of bounds: 'i' is above 'n', the number of "
               "elements of 'p'",
               "22: 'p[i]' is out of bounds: 'i' is at least 'n', the number "
               "of elements of 'p'"}));
}

// An index compared with a constant is bounded above by it (RFC 0011,
// *Relations*): the boundary of a counted loop may reach past a smaller
// object; a guard that fits proves the access; a copy of the index carries
// the bound; and an extent bounded above cannot hold a constant access past
// the bound.
TEST(Bounds, ConstantUpperBounds) {
  const auto result = analyze(std::string(Types) + R"c(
    void counted(void) {
      char buf[4];
      for (int i = 0; i < 8; i++) buf[i] = 0;
      for (int i = 0; i < 4; i++) buf[i] = 0;
      for (int i = 0; i <= 4; i++) buf[i] = 0;
    }
    void guarded(int i) {
      char buf[8];
      if (i >= 0 && i < 8) buf[i] = 0;
      if (i >= 0 && i < 9) buf[i] = 0;
      if (i >= 8) return;
      buf[i] = 0;
    }
    void copied(void) {
      char buf[4];
      for (int i = 0; i < 8; i++) { int j = i; buf[j] = 0; }
    }
    void scaled(void) {
      int ints[4];
      for (int i = 0; i < 5; i++) ints[i] = 0;
    }
    void small_object(int n) {
      if (n > 4) return;
      char *p = malloc(n);
      if (!p) return;
      p[3] = 0;
      p[4] = 0;
      free(p);
    }
    void length(size_t n) {
      char buf[4];
      if (n < 9) memset(buf, 0, n);
      if (n < 5) memset(buf, 0, n);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"3: 'buf[i]' may be out of bounds: 'i' may be 7 in an object "
               "of 4 bytes",
               "5: 'buf[i]' may be out of bounds: 'i' may be 4 in an object "
               "of 4 bytes",
               "10: 'buf[i]' may be out of bounds: 'i' may be 8 in an object "
               "of 8 bytes",
               "16: 'buf[j]' may be out of bounds: 'j' may be 7 in an object "
               "of 4 bytes",
               "20: 'ints[i]' may be out of bounds: 'i' may be 4 in an object "
               "of 16 bytes",
               "27: 'p[4]' is out of bounds: index 4 of an object of 'n' "
               "bytes",
               "32: 'memset' may access past the end of 'buf': 'n' may be 8, "
               "and 'buf' has 4 bytes"}));
}

// A write to the counter or the index forgets what was known about them.
TEST(Bounds, WritesForgetRelationsAndExtents) {
  const auto result = analyze(std::string(Types) + R"c(
    void counter_moves(int n) {
      int *p = malloc(n * sizeof *p);
      if (!p) return;
      n = n + 1;
      p[n - 1] = 0;
      free(p);
    }
    void index_moves(int i, int n) {
      int *p = malloc(n * sizeof *p);
      if (!p) return;
      if (i >= n) {
        i = 0;
        p[i] = 1;
      }
      free(p);
    }
    void copied_index(int i, int n) {
      int *p = malloc(n * sizeof *p);
      if (!p) return;
      int j = i;
      if (j >= n) p[i] = 1;
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"21: 'p[i]' is out of bounds: 'i' is at least 'n', the "
                     "number of elements of 'p'"}));
}

// Library calls with a buffer and a length (RFC 0011, *Library calls*).
TEST(Bounds, LibraryBufferLengthPairs) {
  const auto result = analyze(std::string(Types) + R"c(
    void copy(void) {
      char src[16];
      char *dst = malloc(8);
      if (!dst) return;
      memcpy(dst, src, 8);
      memcpy(dst, src, 16);
      free(dst);
    }
    void read_line(void *f) {
      char line[32];
      if (fgets(line, 32, f)) use(line);
      if (fgets(line, 64, f)) use(line);
    }
    void source_short(void) {
      char big[400];
      char small[200];
      memcpy(big, small, 400);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"6: 'memcpy' accesses 16 bytes of 'dst', which has 8 bytes",
               "12: 'fgets' accesses 64 bytes of 'line', which has 32 bytes",
               "17: 'memcpy' accesses 400 bytes of 'small', which has 200 "
               "bytes"}));
}

// -- Extents in summaries (RFC 0011, *Extents in summaries*) ------------------

// An unconditional access at a constant offset past the pointee's own size,
// or at a symbolic one, is a requirement on the parameter; one under an
// ordering, or at an offset the type already promises, is not.
TEST(ExtentRequirements, InferredFromUnconditionalAccesses) {
  const auto result = analyze(std::string(Types) + R"c(
    static void put7(char *b) { b[7] = 0; }
    static void put_n(char *b, size_t n) { b[n] = 0; }
    static void fill(int *b, int n) { for (int i = 0; i < n; i++) b[i] = 0; }
    static void first(struct outer *o) { o->k = 1; }
    static void guarded(char *b, int n) { if (n > 4) b[4] = 0; }
    static void on_zero(char *b, int n) { if (n == 0) b[7] = 0; }
    static void clears(void *b, size_t n) { memset(b, 0, n); }
    static void put8(char *b) { for (int i = 0; i < 8; i++) b[i] = 0; }
    static void put_le8(char *b) { for (int i = 0; i <= 8; i++) b[i] = 0; }
    static void either(int *b, int n) {
      for (int i = 0; i < n && i < 16; i++) b[i] = 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  const auto requirements = [&result](const char *name) {
    const core::FunctionSummary *s = result.summary(name);
    if (s == nullptr || !s->requiresExtent.contains(0))
      return std::set<core::ExtentRequirement>{};
    return s->requiresExtent.at(0);
  };
  EXPECT_EQ(requirements("put7"),
            (std::set<core::ExtentRequirement>{
                {.need = PathAffine::ofConstant(8), .when = {}}}));
  EXPECT_EQ(requirements("put_n"),
            (std::set<core::ExtentRequirement>{
                {.need = PathAffine::ofPath(SummaryPath::param(1), 1, 1),
                 .when = {}}}));
  EXPECT_EQ(requirements("fill"),
            (std::set<core::ExtentRequirement>{
                {.need = PathAffine::ofPath(SummaryPath::param(1), 4, 0),
                 .when = {}}}))
      << "`b[i]` under `i < n` needs `n` elements at the boundary";
  EXPECT_EQ(requirements("first"), std::set<core::ExtentRequirement>{})
      << "the type promises `sizeof(struct outer)`";
  EXPECT_EQ(requirements("guarded"), std::set<core::ExtentRequirement>{})
      << "no guard spells `n > 4`";
  core::ExtentRequirement onZero{.need = PathAffine::ofConstant(8), .when = {}};
  onZero.when.require(SummaryPath::param(1), core::ValueFact::ofConstant(0));
  EXPECT_EQ(requirements("on_zero"),
            (std::set<core::ExtentRequirement>{onZero}));
  EXPECT_EQ(requirements("clears"),
            (std::set<core::ExtentRequirement>{
                {.need = PathAffine::ofPath(SummaryPath::param(1), 1, 0),
                 .when = {}}}));
  EXPECT_EQ(requirements("put8"),
            (std::set<core::ExtentRequirement>{
                {.need = PathAffine::ofConstant(8), .when = {}}}))
      << "`b[i]` under `i < 8` needs 8 at the boundary";
  EXPECT_EQ(requirements("put_le8"),
            (std::set<core::ExtentRequirement>{
                {.need = PathAffine::ofConstant(9), .when = {}}}));
  EXPECT_EQ(requirements("either"), std::set<core::ExtentRequirement>{})
      << "the smaller of `n` and 16 is not something a summary spells";
}

// A requirement is checked at the call against what the argument has.
TEST(ExtentRequirements, CheckedAtTheCall) {
  const auto result = analyze(std::string(Types) + R"c(
    static void put7(char *b) { b[7] = 0; }
    static void put_n(char *b, size_t n) { b[n] = 0; }
    static void fill(int *b, int n) { for (int i = 0; i < n; i++) b[i] = 0; }
    void calls(void) {
      char small[4];
      char big[8];
      put7(big);
      put7(small);
      char *heap = malloc(4);
      if (!heap) return;
      put_n(heap, 3);
      put_n(heap, 4);
      int ints[4];
      fill(ints, 4);
      fill(ints, 8);
      // Needing nothing is not needing something before the start (Lua's
      // `tablerehash(tb->hash, 0, n)`).
      put_n(heap, -1);
      fill(ints, 0);
      free(heap);
    }
    void at_offset(void) {
      char buf[10];
      put7(buf + 2);
      put7(&buf[3]);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "8: 'put7' requires 8 bytes behind 'small', which has 4 bytes",
          "12: 'put_n' requires 5 bytes behind 'heap', which has 4 bytes",
          "15: 'fill' requires 32 bytes behind 'ints', which has 16 bytes",
          "25: 'put7' requires 11 bytes behind 'buf', which has 10 bytes"}));
  for (const std::string &id : ids(result.diagnostics))
    EXPECT_EQ(id, "out-of-bounds");
}

// -- WEAVEC_SIZED_BY (RFC 0011, *Annotations*) --------------------------------

TEST(SizedBy, GivesAParameterAnExtentAndARequirement) {
  const auto result = analyze(std::string(Types) + R"c(
    void fill(char *SIZED_BY(n) p, size_t n) {
      for (size_t i = 0; i < n; i++) p[i] = 0;
      p[n] = 0;
    }
    void ints(int *SIZED_BY(n) p, int n) { p[n - 1] = 0; }
    void call(void) {
      char buf[4];
      fill(buf, 4);
      fill(buf, 8);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "3: 'p[n]' is out of bounds: 'n' is the number of elements of 'p'",
          "9: 'fill' requires 8 bytes behind 'buf', which has 4 bytes"}));
  EXPECT_EQ(notes(result.diagnostics, 0), Strings{"'p' is declared here"});
  // The annotation is a requirement callers see (the resolved summary).
  const auto resolved = [&result](const char *name) {
    const clang::FunctionDecl *fn = result.function(name);
    const auto found = result.analyzer->summaries().lookup(*fn);
    return found ? found->summary : nullptr;
  };
  const core::FunctionSummary *fill = resolved("fill");
  ASSERT_NE(fill, nullptr);
  ASSERT_TRUE(fill->requiresExtent.contains(0));
  EXPECT_TRUE(fill->requiresExtent.at(0).contains(core::ExtentRequirement{
      .need = PathAffine::ofPath(SummaryPath::param(1), 1, 0), .when = {}}));
  const core::FunctionSummary *ints = resolved("ints");
  ASSERT_NE(ints, nullptr);
  ASSERT_TRUE(ints->requiresExtent.contains(0));
  EXPECT_TRUE(ints->requiresExtent.at(0).contains(core::ExtentRequirement{
      .need = PathAffine::ofPath(SummaryPath::param(1), 4, 0), .when = {}}))
      << "in units of the pointee";
}

TEST(SizedBy, MalformedAnnotationsAreReported) {
  const auto result = analyze(std::string(Types) + R"c(
    void not_pointer(int SIZED_BY(n) x, int n) { (void)x; (void)n; }
    void not_counter(char *SIZED_BY(q) p, char *q) { (void)p; (void)q; }
    void no_such(char *SIZED_BY(m) p, int n) { (void)p; (void)n; }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{
          "1: 'x' is declared WEAVEC_SIZED_BY(n) but is not a pointer",
          "2: 'p' is declared WEAVEC_SIZED_BY(q) but 'q' is not an integer "
          "parameter",
          "3: 'p' is declared WEAVEC_SIZED_BY(m) but 'm' is not an integer "
          "parameter"}));
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"invalid-annotation", "invalid-annotation",
                     "invalid-annotation"}));
}

// -- Whole-program (RFC 0011, *Extents in summaries*) -------------------------

// Extents and requirements cross the summary: a wrapper's caller knows the
// size, and a callee's need is checked against a caller's allocation.
TEST(ExtentRequirements, ComposeThroughWrappers) {
  const auto result = analyze(std::string(Types) + R"c(
    static char *xmalloc(size_t n) {
      char *p = malloc(n);
      if (!p) __builtin_trap();
      return p;
    }
    static void put7(char *b) { b[7] = 0; }
    static void via_wrapper(void) {
      char *p = xmalloc(4);
      p[4] = 0;
      put7(p);
      free(p);
    }
    static void deeper(char *b) { put7(b); }
    static void via_deeper(void) {
      char *p = xmalloc(4);
      deeper(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"9: 'p[4]' is out of bounds: index 4 of an object of 4 bytes",
               "10: 'put7' requires 8 bytes behind 'p', which has 4 bytes",
               "16: 'deeper' requires 8 bytes behind 'p', which has 4 bytes"}));
}

} // namespace
} // namespace weavec::analysis
