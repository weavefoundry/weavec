// RFC 0009, *Deriving guards*, *Replaced values under a guard*: a value gone
// at the callee's exit only under a guard was replaced (or never consumed)
// on every other returning path, so the unreplaced consume the caller must
// see applies under that guard. RFC 0008, *Replaced values*: a replaced
// consume that finds the value already gone is one report, after which the
// place holds the callee's new value.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

struct L {
  char *stack;
};

struct W {
  struct L *L;
};

static void finish(struct L *L) { free(L->stack); }

static void append(struct L *L, const char *b) {
  free(L->stack);
  L->stack = malloc(8);
  use(b);
}

// Lua's `str_writer`: a null block finishes (the stack is freed for good),
// anything else appends (the stack is freed and replaced). The consume the
// caller sees is guarded on the null; the store keeps its own guard.
// DUMP-LABEL: function 'writer':
// DUMP: summary: L->stack: written|freed(free) when[b null]; *b: read; stores{L->stack = fresh(free) extent=8 when[b nonnull], L->stack = null when[b nonnull]} returns{} requires{L}
void writer(struct L *L, const char *b) {
  if (b == NULL)
    finish(L);
  else
    append(L, b);
}

// A buffer is never null: neither call frees the caller's stack.
void twice(struct L *L) {
  char buf[4];
  writer(L, buf);
  writer(L, buf);
  use(L->stack);
}

void twice_null(struct L *L) {
  writer(L, NULL);
  // CHECK: [[@LINE+1]]:3: error: 'L->stack' is freed twice [weavec::double-free]
  writer(L, NULL);
}

void unknown(struct L *L, const char *b) {
  writer(L, b);
  // CHECK: [[@LINE+1]]:3: error: 'L->stack' is freed twice [weavec::double-free]
  writer(L, b);
}

// `via` consumes `L->stack` through a local alias and its callee stores a
// new value there: the caller's place is replaced, but no store names it in
// the caller's terms.
// DUMP-LABEL: function 'via':
// DUMP: summary: L->stack: written|freed(free)|replaced; stores{} returns{}
static void through(struct W *w) {
  free(w->L->stack);
  w->L->stack = malloc(8);
}

static void via(struct L *L) {
  struct W w;
  w.L = L;
  through(&w);
}

// One report, at the first call after the free; the calls after it use the
// value `via` left.
// DUMP-LABEL: function 'cascade':
// DUMP: summary: L->stack: written|freed(free)|replaced; stores{} returns{} requires{L}
void cascade(struct L *L) {
  free(L->stack);
  // CHECK: [[@LINE+1]]:3: error: 'L->stack' is freed twice [weavec::double-free]
  via(L);
  via(L);
  via(L);
}
// CHECK-NOT: error:
