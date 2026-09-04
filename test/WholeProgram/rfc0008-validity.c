// RFC 0008 across units (RFC 0005): `replaced`, `requires`, `null` returns,
// `notnull` outcomes and interior results travel in the program database, so
// a caller in this unit is checked against definitions in another.
//
// RUN: not %weavec --whole-program %s %S/Inputs/validity.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/validity.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=DUMP %s
#include <stdlib.h>
#include <string.h>
#include "validity.h"

// DUMP: function 'find': param 0 *: read; stores{} returns{interior param 0, null} requires{param 0}
// DUMP: function 'node_open': param 0 *: read,written; stores{param 0 * = fresh(free), param 0 * = null} returns{} requires{param 0} outcome zero{} null{param 0 *} outcome positive{} notnull{param 0 *}
// DUMP: function 'node_value': param 0 *.value: read; stores{} returns{} requires{param 0}
// DUMP: function 'vec_grow': param 0 *.cap: read,written; param 0 *.items: written,moved(free),replaced; stores{param 0 *.items = fresh(free)} returns{} requires{param 0} outcome zero{} null{param 0 *.items} stored{} outcome positive{param 0 *.items: moved(free),replaced} notnull{param 0 *.items} stored{param 0 *.items}
// DUMP: function 'vec_reset': param 0 *.items: written,freed(free),replaced; stores{param 0 *.items = null} returns{} requires{param 0}

int replaced_copy(struct vec *v) {
  int *old = v->items;
  if (!vec_grow(v))
    return 1;
  // CHECK: rfc0008-validity.c:[[@LINE+1]]:10: error: use of 'old' after it was moved [weavec::use-after-move]
  return old[0];
}

int reset_copy(struct vec *v) {
  int *old = v->items;
  vec_reset(v);
  // CHECK: rfc0008-validity.c:[[@LINE+1]]:10: error: use of 'old' after it was freed [weavec::use-after-free]
  return old[0];
}

int null_result(const char *s) {
  char *p = find(s, 'x');
  // CHECK: rfc0008-validity.c:[[@LINE+2]]:11: error: dereference of 'p', which may be null [weavec::null-dereference]
  // CHECK: rfc0008-validity.c:[[@LINE-2]]:13: note: 'p' may be null: it is the result of 'find' here
  return *p;
}

int passes_maybe_null(void) {
  struct node *n = malloc(sizeof *n);
  // CHECK: rfc0008-validity.c:[[@LINE+1]]:22: error: 'n', which may be null, is passed to 'node_value', which dereferences it [weavec::null-dereference]
  int v = node_value(n);
  free(n);
  return v;
}

void interior_release(const char *t) {
  char *s = strdup(t);
  if (!s)
    return;
  char *p = find(s, 'x');
  if (!p) {
    free(s);
    return;
  }
  // CHECK: rfc0008-validity.c:[[@LINE+1]]:3: error: 'p' is released but does not point to the start of its allocation [weavec::invalid-release]
  free(p);
}

// Clean: the outcome of `node_open` proves `*out` non-null; the grown vector
// is used through the place, not through a stale copy.
int fine(struct vec *v) {
  struct node *n;
  if (!node_open(&n))
    return 1;
  int r = node_value(n);
  free(n);
  if (!vec_grow(v))
    return 1;
  v->items[0] = r;
  vec_reset(v);
  return 0;
}

// CHECK: 5 errors generated.
