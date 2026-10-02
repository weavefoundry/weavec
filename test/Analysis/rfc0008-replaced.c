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
  // RFC 0031 §5.11: the note comes from the release record, which a callee's
  // summary effect makes at the call without the name it released through.
  // CHECK: rfc0008-replaced.c:[[@LINE+2]]:10: error: use of 'old' after it was moved [weavec::use-after-move]
  // CHECK: rfc0008-replaced.c:[[@LINE-5]]:8: note: moved here
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
  // CHECK: rfc0008-replaced.c:[[@LINE-2]]:3: note: freed here
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
  // CHECK: rfc0008-replaced.c:[[@LINE+2]]:3: warning: 'p.a' is leaked [weavec::leak]
  // CHECK: rfc0008-replaced.c:[[@LINE-3]]:19: note: allocated here
  return 0;
}

int tidy(void) {
  struct pair p = make();
  free(p.a);
  free(p.b);
  return 0;
}

// The summary vocabulary (RFC 0031 §6.1, format 30): a release or move
// per result class, a store of the replacement, `result` as a store root.
// DUMP: function 'grow':
// The success class moves the items into the new block; the failure class
// keeps them (RFC 0030 §8.2's release of a zero-size request does not
// arise: the zero-initialisation wrapper asks for one byte instead).
// DUMP: move *param0->items free when result positive
// DUMP: store param0->items := fresh#0 free {{.*}}when result positive
// DUMP: function 'reset':
// DUMP: release *param0->items free when always
// DUMP-NEXT: store param0->items := null
// DUMP: function 'make':
// DUMP: store result.a := fresh#0 free extent 4
// DUMP-NEXT: store result.b := fresh#1 free extent 4

// CHECK: 1 warning and 2 errors generated.
