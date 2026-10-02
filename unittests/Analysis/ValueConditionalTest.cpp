//===- ValueConditionalTest.cpp - Scalar facts, guards, noreturn ----------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0009: value-conditional behaviour. Each test names the RFC section it
// pins.
//
//===----------------------------------------------------------------------===//

#include "TestUtils.h"
#include "weavec/Analysis/ProgramDatabase.h"

#include <gtest/gtest.h>

namespace weavec::analysis {

namespace {

using core::SummaryPath;
using test::analyze;
using test::analyzeInProgram;
using test::ids;
using test::messages;

using Strings = std::vector<std::string>;

constexpr const char *Abort = R"c(
void abort(void);
#line 1
)c";

// -- Scalar facts (RFC 0009, *Scalar facts in the state*) ---------------------

TEST(ValueConditional, UncorrelatedTestsStillReport) {
  const auto result = analyze(R"c(
    void overlap(int n, char *p) {
      if (n > 0) free(p);
      if (n != 0) use(p);
    }
    void reassigned(int c, int d, char *p) {
      if (c) free(p);
      c = d;
      if (!c) use(p);
    }
    void other_variable(int c, int d, char *p) {
      if (c) free(p);
      if (!d) use(p);
    }
    void computed(int n, char *p) {
      if (n == 0) free(p);
      if ((n & 1) != 0) use(p);
    }
    void copied_variable(int c, char *p) {
      int d;
      if (c) free(p);
      d = c;
      if (!d) use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics),
            (Strings{"use-after-free", "use-after-free", "use-after-free",
                     "use-after-free", "use-after-free"}));
  // `copied_variable`: `d = c` copies a fact, not a relation; with nothing
  // known about `c` at the copy the later test says nothing about the move.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"4: use of 'p' after it may have been freed",
                     "9: use of 'p' after it may have been freed",
                     "13: use of 'p' after it may have been freed",
                     "17: use of 'p' after it may have been freed",
                     "23: use of 'p' after it may have been freed"}));
}

// RFC 0009, *Assumptions*: a class is that of the mathematical value, and a
// comparison is decided in its operands' common type. A constant the type
// reads differently from its bits (`ULONG_MAX`, `(size_t)-1`) must not turn
// a live edge infeasible; cJSON's `if (index > ULONG_MAX)` guard lost the
// `null-dereference` that followed it.
TEST(ValueConditional, UnsignedComparisonsAreDecidedInTheirType) {
  const auto result = analyze(R"c(
    static void touch(char *p) { p[0] = 1; }
    void above_max(void) {
      size_t i = 0;
      char *p = malloc(4);
      if (i > 18446744073709551615UL) { free(p); return; }
      touch(p);
      free(p);
    }
    void sentinel(void) {
      size_t n = (size_t)-1;
      char *p = malloc(4);
      if (n == (size_t)-1) { free(p); return; }
      touch(p);
      free(p);
    }
    void minus_one_is_uint_max(void) {
      unsigned x = -1;
      char *p = malloc(4);
      if (x > 5) touch(p);
      free(p);
    }
    void negative_ranks_above(void) {
      int x = -1;
      char *p = malloc(4);
      if (x > 5u) touch(p);
      free(p);
    }
    void case_minus_one(void) {
      unsigned x = 4294967295U;
      char *p = malloc(4);
      switch (x) { case -1: touch(p); break; default: break; }
      free(p);
    }
    void promoted(void) {
      unsigned char x = -1;
      char *p = malloc(4);
      if (x == 255) touch(p);
      free(p);
    }
    void dead(void) {
      size_t i = 0;
      char *p = malloc(4);
      if (i != 0) touch(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // RFC 0017: `dead` and unsigned comparison with zero both have dead edges.
  // RFC 0030 §3.2: `touch`'s requirement is a may-fact, so a pointer that
  // may be null passed to it is no diagnostic either way.
  EXPECT_EQ(messages(result.diagnostics), (Strings{}));
}

TEST(ValueConditional, GuardedResourcesAreNotLeakedOnRefutedEdges) {
  const auto result = analyze(R"c(
    int merged(int c) {
      char *p = NULL;
      if (c) p = malloc(8);
      if (!c) return -1;
      free(p);
      return 0;
    }
    int sized(size_t n) {
      char *p = NULL;
      if (n > 0) p = malloc(n);
      if (n == 0) return 0;
      use(p);
      free(p);
      return 1;
    }
    int leaked(int c) {
      char *p = NULL;
      if (c) p = malloc(8);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(ids(result.diagnostics), (Strings{"leak"}));
  // RFC 0031 §5.8: at the `return` that drops it.
  EXPECT_EQ(messages(result.diagnostics), (Strings{"19: 'p' is leaked"}));
}

// -- Argument-conditional summaries (RFC 0009, *Deriving guards*) -------------

constexpr const char *Alloc = R"c(
void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud; (void)osize;
  if (nsize == 0) { free(ptr); return NULL; }
  return realloc(ptr, nsize);
}
struct buf { char *data; int noalloc; };
void release(struct buf *b) {
  if (!b->noalloc) free(b->data);
}
struct state { char *msg; int err; };
void gz_error(struct state *s, int err, char *msg) {
  s->err = err;
  if (msg != NULL) s->msg = msg;
}
#line 1
)c";

TEST(ValueConditional, CallersSelectEffectsByArgument) {
  const auto result = analyze(std::string(Alloc) + R"c(
    void grow(void *ud) {
      char *p = malloc(8);
      char *q = l_alloc(ud, p, 8, 16);
      use(q);
      free(q);
    }
    void shrink(void *ud) {
      char *p = malloc(8);
      l_alloc(ud, p, 8, 0);
      use(p);
    }
    void unknown_size(void *ud, size_t n) {
      char *p = malloc(8);
      char *q = l_alloc(ud, p, 8, n);
      use(p);
      free(q);
    }
    void keep_static(void) {
      char stack[8];
      struct buf b;
      b.data = stack;
      b.noalloc = 1;
      release(&b);
      use(b.data);
    }
    void free_heap(void) {
      struct buf b;
      b.data = malloc(8);
      b.noalloc = 0;
      release(&b);
      use(b.data);
    }
    void unknown_flag(struct buf *b) {
      release(b);
      use(b->data);
    }
    void store_null(struct state *s) { gz_error(s, 1, NULL); }
    void store_local(struct state *s) {
      char local[8];
      gz_error(s, 1, local);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{
                // `shrink`: the discarded result is null, not a leak; the
                // block was freed: `nsize` is 0, which selects the free
                // (RFC 0030 §9.1, `param 3 =0`).
                "11: use of 'p' after it was freed",
                // `unknown_size`: freed, or moved by `realloc`.
                "16: use of 'p' after it may have been moved",
                // `keep_static`, `free_heap`: the numeric context of each
                // call binds `b.noalloc` (RFC 0031 §6.6), so `release`
                // frees nothing in the first and frees `b.data` in the
                // second.
                "32: use of 'b.data' after it was freed",
                // `unknown_flag`: nothing known about the flag.
                "36: use of 'b->data' after it may have been freed",
                // `store_local`: the store happens for a non-null argument.
                "41: 's->msg' may outlive 'local', which it points to",
            }));
}

// -- Guarded outcome classes (RFC 0009, *Guards*) -----------------------------

// -- Replaced values under a guard (RFC 0009, *Deriving guards*) --------------

constexpr const char *Writer = R"c(
    struct L { char *stack; };
    static void finish(struct L *L) { free(L->stack); }
    static void append(struct L *L, const char *b) {
      free(L->stack);
      L->stack = malloc(8);
      use(b);
    }
    // Lua's `str_writer`: one arm frees the stack for good, the other frees
    // and replaces it.
    void writer(struct L *L, const char *b) {
      if (b == NULL) finish(L);
      else append(L, b);
    }
)c";

TEST(ValueConditional, CallersOfAGuardedUnreplacedConsumeSelectByArgument) {
  const auto result = analyze(std::string(Writer) + R"c(
    void twice(struct L *L) {
      char buf[4];
      writer(L, buf);
      writer(L, buf);
      use(L->stack);
    }
    void twice_null(struct L *L) {
      writer(L, NULL);
      writer(L, NULL);
    }
    void unknown(struct L *L, const char *b) {
      writer(L, b);
      writer(L, b);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{
                // `twice_null`: the null argument selects the freeing arm.
                "24: 'L->stack' is freed twice",
                // `unknown`: nothing known about `b`, so it may.
                "28: 'L->stack' may be freed twice",
            }));
}

// RFC 0031 *Pending cases and exit splitting*: a release made where a local
// was non-null, on a path that returns that local, is keyed to the non-null
// result, so a caller that tests the result frees the old block once.
TEST(ValueConditional, ReleasesGuardedByTheReturnedLocalKeyToItsClass) {
  const auto result = analyze(R"c(
    void *grow(void *ptr, size_t n) {
      void *m = malloc(n);
      if (m && ptr) free(ptr);
      return m;
    }
    void caller(char *b) {
      char *q = grow(b, 16);
      if (!q) { free(b); return; }
      free(q);
    }
    void wrong(char *b) {
      char *q = grow(b, 16);
      if (q) { free(b); free(q); }
    }
  )c");
  ASSERT_TRUE(result.ast);
  const core::FunctionEffects *summary = result.summary("grow");
  ASSERT_NE(summary, nullptr);
  ASSERT_EQ(summary->effects.size(), 1U);
  EXPECT_EQ(summary->effects[0].when.classes,
            (std::vector<core::ResultClass>{core::ResultClass::NonNull}));
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"14: 'b' may be freed twice"}));
}

// RFC 0031 §6.3: `return *out != NULL` splits the exit by the comparison; on
// the zero class the allocation was never made, so the caller's failing
// branch leaks nothing and the other branch's `n` is non-null.
TEST(ValueConditional, AnOutParameterAllocationIsAbsentOnTheFailingClass) {
  const auto result = analyze(R"c(
    struct node { int value; };
    static int open_node(struct node **out) {
      *out = malloc(sizeof **out);
      return *out != NULL;
    }
    int outcome(void) {
      struct node *n;
      if (!open_node(&n))
        return 1;
      n->value = 2;
      free(n);
      return 0;
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_TRUE(result.diagnostics.empty())
      << ::testing::PrintToString(messages(result.diagnostics));
  const core::FunctionEffects *summary = result.summary("open_node");
  ASSERT_NE(summary, nullptr);
  ASSERT_EQ(summary->stores.size(), 1U);
  EXPECT_EQ(summary->stores[0].absentOn,
            (std::vector<core::ResultClass>{core::ResultClass::Zero}));
}

// -- Inferred `noreturn` (RFC 0009, *Inferred `noreturn`*) --------------------

TEST(ValueConditional, NeverReturnsIsInferredTransitively) {
  const auto result = analyze(std::string(Abort) + R"c(
    static void die(const char *msg) { use(msg); abort(); }
    static void fail(int code) { if (code > 3) die("big"); die("small"); }
    static void check(int ok) { if (!ok) die("bad"); }
    static void spin(void) { for (;;) ; }
    static void loop_out(int n) { while (n) n--; }
    void good_path(int bad) {
      char *q = malloc(8);
      if (bad) { free(q); fail(bad); }
      use(q);
      free(q);
    }
    void check_returns(int bad) {
      char *q = malloc(8);
      if (bad) { free(q); check(bad); }
      use(q);
      free(q);
    }
    void no_leak_after_die(int bad) {
      char *q = malloc(8);
      if (bad) die("bad");
      free(q);
    }
    void dead_tail(char *p) {
      free(p);
      die("x");
      use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"16: use of 'q' after it may have been freed",
                     "17: 'q' may be freed twice"}));
}

TEST(ValueConditional, NeverReturnsThroughFunctionPointersNeedsEveryCandidate) {
  // Candidates of an indirect call are the address-taken functions of its
  // type (RFC 0004): with `die` alone the call never returns.
  const auto alone = analyze(std::string(Abort) + R"c(
    static void die(const char *msg) { use(msg); abort(); }
    void (*const handler)(const char *) = die;
    void via_handler(char *p) {
      free(p);
      handler("x");
      use(p);
    }
  )c");
  ASSERT_TRUE(alone.ast);
  EXPECT_EQ(messages(alone.diagnostics), Strings{});

  // A candidate that returns, even one that does nothing at all, makes the
  // call return.
  const auto either = analyze(std::string(Abort) + R"c(
    static void die(const char *msg) { use(msg); abort(); }
    static void ignore(const char *msg) {}
    void (*const handlers[2])(const char *) = {ignore, die};
    void via_handler(char *p, int i) {
      free(p);
      handlers[i]("x");
      use(p);
    }
  )c");
  ASSERT_TRUE(either.ast);
  EXPECT_EQ(messages(either.diagnostics),
            (Strings{"8: use of 'p' after it was freed"}));
}

} // namespace
} // namespace weavec::analysis
