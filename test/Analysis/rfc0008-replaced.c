// RFC 0008, *Replaced values*: a callee that releases a caller-visible value
// and then reinitialises the place (`realloc`, `free` + `= NULL`) still
// consumed the caller's *old* value; a copy of it the caller kept is dead.
// Also *Struct-by-value results*: the pointer fields of a returned record are
// tracked in the caller through the `result` summary root.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include <stdlib.h>

struct vec {
  int *items;
  int cap;
};

// The RFC's motivating hole: the copy of the old items is used after `grow`
// moved it into `realloc`.
static int grow(struct vec *v) {
  int *bigger = realloc(v->items, sizeof *bigger * (v->cap + 8));
  if (!bigger)
    return 0;
  v->items = bigger;
  v->cap += 8;
  return 1;
}

int hole(struct vec *v) {
  int *old = v->items;
  if (!grow(v))
    return 1;
  // CHECK: rfc0008-replaced.c:[[@LINE+2]]:10: error: use of 'old' after it was moved [weavec::use-after-move]
  // CHECK: rfc0008-replaced.c:[[@LINE-3]]:8: note: moved here (through 'v->items')
  return old[0];
}

// `free` then `= NULL`: the same, with `freed`.
static void reset(struct vec *v) {
  free(v->items);
  v->items = NULL;
}

int reset_hole(struct vec *v) {
  int *old = v->items;
  reset(v);
  // CHECK: rfc0008-replaced.c:[[@LINE+2]]:10: error: use of 'old' after it was freed [weavec::use-after-free]
  // CHECK: rfc0008-replaced.c:[[@LINE-2]]:3: note: freed here (through 'v->items')
  return old[0];
}

// Clean: the place itself holds the replacement, so using it is fine; a
// `replaced` path is not `freed` for the field's own next use.
int clean(struct vec *v) {
  if (!grow(v))
    return 1;
  v->items[0] = 1;
  reset(v);
  if (v->items)
    return v->items[0];
  return 0;
}

// Struct-by-value results: the caller owns `p.a` and `p.b` and must release
// both.
struct pair {
  char *a;
  char *b;
};

static struct pair make(void) {
  struct pair p;
  p.a = malloc(4);
  p.b = malloc(4);
  return p;
}

int leaky(void) {
  struct pair p = make();
  free(p.b);
  // CHECK: rfc0008-replaced.c:[[@LINE+2]]:10: warning: 'p.a' is leaked [weavec::leak]
  // CHECK: rfc0008-replaced.c:[[@LINE-3]]:19: note: allocated here
  return 0;
}

int tidy(void) {
  struct pair p = make();
  free(p.a);
  free(p.b);
  return 0;
}

// The summary vocabulary (RFC 0008, *Summary text format*): `replaced` among
// the flags, `result` as a store root.
// DUMP: function 'grow':
// DUMP: summary: v->cap: read|written; v->items: written|moved(free)|replaced; stores{v->items = fresh(free)} returns{} requires{v} outcome zero{} null{v->items} stored{} outcome positive{v->items: moved(free) replaced} notnull{v->items} stored{v->items}
// DUMP: function 'reset':
// DUMP: summary: v->items: written|freed(free)|replaced; stores{v->items = null} returns{} requires{v}
// DUMP: function 'make':
// DUMP: summary: stores{result.a = fresh(free), result.b = fresh(free)} returns{}

// CHECK: 1 warning and 2 errors generated.
