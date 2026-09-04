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
#include "weavec/Core/Scalar.h"

#include <gtest/gtest.h>

namespace weavec::analysis {

/// A guard of the one conjunct "`path` satisfies `fact`".
static core::PathGuard when(const core::SummaryPath &path,
                            const core::ValueFact &fact) {
  core::PathGuard guard;
  guard.require(path, fact);
  return guard;
}

namespace {

using core::Outcome;
using core::PathGuard;
using core::SummaryPath;
using core::ValueFact;
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

TEST(ValueConditional, CorrelatedTestsOfOneIntegerAreOneTest) {
  const auto result = analyze(R"c(
    void truthy(int c, char *p) {
      if (c) free(p);
      if (!c) use(p);
    }
    void eq(int n, char *p) {
      if (n == 0) free(p);
      if (n != 0) use(p);
    }
    void sign(int n, char *p) {
      if (n > 0) free(p);
      if (n <= 0) use(p);
    }
    void constant(int n, char *p) {
      if (n == 3) free(p);
      if (n == 4) use(p);
    }
    void switched(int n, char *p) {
      switch (n) { case 0: free(p); break; default: break; }
      switch (n) { case 1: use(p); break; default: break; }
    }
    void local_constant(char *p) {
      int c = 0;
      if (c) free(p);
      use(p);
      free(p);
    }
    void copied_constant(char *p) {
      int c = 0;
      int d = c;
      if (d) free(p);
      use(p);
      free(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});

  // The moves are recorded under their guards.
  const core::FunctionSummary *eq = result.summary("eq");
  ASSERT_NE(eq, nullptr);
  EXPECT_EQ(eq->effectOf(SummaryPath::param(1)).when,
            when(SummaryPath::param(0), ValueFact::ofConstant(0)));
  EXPECT_EQ(result.summary("sign")->effectOf(SummaryPath::param(1)).when,
            when(SummaryPath::param(0), ValueFact::of(Outcome::Positive)));
  EXPECT_EQ(result.summary("truthy")->effectOf(SummaryPath::param(1)).when,
            when(SummaryPath::param(0), ValueFact::nonZero()));
  EXPECT_EQ(result.summary("constant")->effectOf(SummaryPath::param(1)).when,
            when(SummaryPath::param(0), ValueFact::ofConstant(3)));
  EXPECT_TRUE(result.summary("local_constant")
                  ->effectOf(SummaryPath::param(0))
                  .when.trivial())
      << "a fact about a local is no condition on the caller";
}

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
            (Strings{"4: use of 'p' after it was freed",
                     "9: use of 'p' after it was freed",
                     "13: use of 'p' after it was freed",
                     "17: use of 'p' after it was freed",
                     "23: use of 'p' after it was freed"}));
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
  // Every `touch` but `dead`'s is on a live edge.
  const auto mayBeNull = [](const char *line) {
    return std::string(line) +
           ": 'p', which may be null, is passed to 'touch', which "
           "dereferences it";
  };
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{mayBeNull("7"), mayBeNull("14"), mayBeNull("20"),
                     mayBeNull("26"), mayBeNull("32"), mayBeNull("38")}));
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
  EXPECT_EQ(messages(result.diagnostics), (Strings{"20: 'p' is leaked"}));
}

TEST(ValueConditional, FactsAboutCallerMemoryAndBorrowedLocals) {
  const auto result = analyze(R"c(
    struct buf { char *data; int owned; };
    void field(struct buf *b) {
      if (b->owned) free(b->data);
      if (!b->owned) use(b->data);
    }
    void through_pointer(char *p) {
      int c = 0;
      int *q = &c;
      *q = 1;
      if (c) free(p);
      use(p);
    }
    void after_call(struct buf *b, char *p) {
      b->owned = 0;
      field(b);
      if (b->owned) use(p);
    }
  )c");
  ASSERT_TRUE(result.ast);
  // `field`: clean. `through_pointer`: the write through `q` lands in `c`,
  // so `if (c)` is not dead and the use is reported. `after_call`: `field`
  // reads but does not write `b->owned`, so the fact survives the call and
  // `use(p)` is dead code; nothing is reported there.
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"12: use of 'p' after it was freed"}));
  EXPECT_EQ(
      result.summary("field")
          ->effectOf(SummaryPath::param(0).deref().field("data"))
          .when,
      when(SummaryPath::param(0).deref().field("owned"), ValueFact::nonZero()));
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

TEST(ValueConditional, SummariesCarryArgumentGuards) {
  const auto result = analyze(Alloc);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});

  const core::FunctionSummary *alloc = result.summary("l_alloc");
  ASSERT_NE(alloc, nullptr);
  // Both arms consume `ptr` (`free` or `realloc`), so the effect itself is
  // unconditional; the fresh result exists only for a non-zero size.
  EXPECT_TRUE(alloc->effectOf(SummaryPath::param(1)).consumed());
  EXPECT_TRUE(alloc->effectOf(SummaryPath::param(1)).when.trivial());
  ASSERT_EQ(alloc->returns.size(), 2U);
  for (const core::ValueSource &source : alloc->returns) {
    if (source.isFresh())
      EXPECT_EQ(source.when, when(SummaryPath::param(3), ValueFact::nonZero()));
    else
      EXPECT_TRUE(source.when.trivial()) << "null on either arm";
  }

  const core::FunctionSummary *release = result.summary("release");
  ASSERT_NE(release, nullptr);
  EXPECT_EQ(release->effectOf(SummaryPath::param(0).deref().field("data")).when,
            when(SummaryPath::param(0).deref().field("noalloc"),
                 ValueFact::ofConstant(0)));

  const core::FunctionSummary *error = result.summary("gz_error");
  ASSERT_NE(error, nullptr);
  ASSERT_EQ(error->stores.size(), 1U);
  EXPECT_EQ(error->stores.begin()->value.when,
            when(SummaryPath::param(2), ValueFact::of(Outcome::NonNull)));
}

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
                // block was freed.
                "11: use of 'p' after it was freed",
                // `unknown_size`: may have been freed.
                "16: use of 'p' after it was freed",
                // `free_heap`: the flag selects the free.
                "32: use of 'b.data' after it was freed",
                // `unknown_flag`: nothing known about the flag.
                "36: use of 'b->data' after it was freed",
                // `store_local`: the store happens for a non-null argument.
                "41: 's->msg' may outlive 'local', which it points to",
            }));
}

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

TEST(ValueConditional, ConsumeUnreplacedOnlyUnderTheExitRecordsGuard) {
  const auto result = analyze(Writer);
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics), Strings{});

  // The value is gone at the exit only when `b` is null: the unreplaced
  // consume the caller must see applies under that guard, and the replaced
  // consume of the other arm is not claimed. Joining the arms into an
  // unconditional, unreplaced consume would tell a caller passing a buffer
  // that its stack was freed while the store that reinitialises it stays
  // guarded on `b null`.
  const core::FunctionSummary *writer = result.summary("writer");
  ASSERT_NE(writer, nullptr);
  const core::PlaceEffect stack =
      writer->effectOf(SummaryPath::param(0).deref().field("stack"));
  EXPECT_TRUE(stack.freed);
  EXPECT_FALSE(stack.replaced);
  EXPECT_EQ(stack.when,
            when(SummaryPath::param(1), ValueFact::of(Outcome::Null)));
}

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
                "28: 'L->stack' is freed twice",
            }));
}

TEST(ValueConditional, ReplacedConsumeAfterAReportLeavesTheNewValue) {
  // `via` frees `L->stack` through a local alias and its callee stores a
  // new value there, so the caller's place is replaced but no store names
  // it in the caller's terms (RFC 0008, *Replaced values*). The first call
  // after the free is a double-free; the calls after it use the value
  // `via` left, and are not.
  const auto result = analyze(R"c(
    struct L { char *stack; };
    struct W { struct L *L; };
    static void through(struct W *w) { free(w->L->stack); w->L->stack = malloc(8); }
    static void via(struct L *L) { struct W w; w.L = L; through(&w); }
    void cascade(struct L *L) {
      free(L->stack);
      via(L);
      via(L);
      via(L);
    }
  )c");
  ASSERT_TRUE(result.ast);
  EXPECT_EQ(messages(result.diagnostics),
            (Strings{"8: 'L->stack' is freed twice"}));

  const core::FunctionSummary *via = result.summary("via");
  ASSERT_NE(via, nullptr);
  EXPECT_TRUE(
      via->effectOf(SummaryPath::param(0).deref().field("stack")).replaced);
  EXPECT_TRUE(via->stores.empty());
  // The caller's own summary agrees: the value it freed was replaced.
  const core::FunctionSummary *cascade = result.summary("cascade");
  ASSERT_NE(cascade, nullptr);
  EXPECT_TRUE(
      cascade->effectOf(SummaryPath::param(0).deref().field("stack")).replaced);
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
  EXPECT_TRUE(result.summary("die")->neverReturns);
  EXPECT_TRUE(result.summary("fail")->neverReturns);
  EXPECT_TRUE(result.summary("spin")->neverReturns);
  EXPECT_FALSE(result.summary("check")->neverReturns);
  EXPECT_FALSE(result.summary("loop_out")->neverReturns);
  EXPECT_FALSE(result.summary("good_path")->neverReturns);
  EXPECT_EQ(
      messages(result.diagnostics),
      (Strings{"16: use of 'q' after it was freed", "17: 'q' is freed twice"}));
}

TEST(ValueConditional, NeverReturnsCrossesUnits) {
  const auto unit = analyze(std::string(Abort) + R"c(
    void die(const char *msg) { use(msg); abort(); }
    void fail(int code) { if (code) die("a"); die("b"); }
  )c");
  ASSERT_TRUE(unit.ast);
  ProgramDatabase db;
  db.add(unit.analyzer->exports());
  ASSERT_NE(db.find("fail"), nullptr);
  EXPECT_TRUE(db.find("fail")->neverReturns);

  const auto client = analyzeInProgram(R"c(
    void die(const char *msg);
    void fail(int code);
    void good(int bad) {
      char *q = malloc(8);
      if (bad) { free(q); fail(bad); }
      use(q);
      free(q);
    }
  )c",
                                       &db);
  ASSERT_TRUE(client.ast);
  EXPECT_EQ(messages(client.diagnostics), Strings{});
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
