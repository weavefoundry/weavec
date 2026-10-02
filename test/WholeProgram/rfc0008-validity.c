// RFC 0008 across units (RFC 0005): `replaced`, `requires`, `null` returns,
// `notnull` outcomes and interior results travel in the program database, so
// a caller in this unit is checked against definitions in another.
//
// RUN: not %weavec --whole-program %s %S/Inputs/validity.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --ledger=%t.json %s %S/Inputs/validity.c -- -I%S/Inputs 2>/dev/null
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/validity.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=DUMP %s
#include <stdlib.h>
#include <string.h>
#include "validity.h"

// The format-30 summaries (RFC 0031 *Summary format 30*): a null result is
// a result class, `requires{param 0}` is `nonnull-on` every class, and
// `replaced` is a store. `strchr`'s result points into `s` at an offset no
// format-30 value spells, so `find` returns `unknown` (RFC 0031 §6.1).
// DUMP: program:
// DUMP: function 'find':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result unknown maybe-null when null nonnull
// DUMP: function 'node_open':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result int [0, 0] when zero and param 0 !=0
// DUMP-NEXT: result int [1, 1] when positive and param 0 !=0
// DUMP-NEXT: store *param0 := fresh#0 free extent 4 {{.*}}maybe-null absent on zero
// DUMP: function 'node_value':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result int
// DUMP-NEXT: nonnull-on zero param0
// DUMP-NEXT: nonnull-on positive param0
// DUMP-NEXT: nonnull-on negative param0
// The failure class keeps the items (RFC 0030 §8.2's release of a size that
// wraps to zero does not arise through the zero-initialisation wrapper);
// the success class moves them and stores the new block.
// DUMP: function 'vec_grow':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result int [0, 0] when zero and param 0 !=0
// DUMP-NEXT: result int [1, 1] when positive and param 0 !=0
// DUMP-NEXT: move *param0->items free when result positive
// DUMP-NEXT: store param0->cap := {{.*}} when result positive
// DUMP-NEXT: store param0->items := fresh#0 free {{.*}}when result positive
// DUMP-NEXT: nonnull-on zero param0
// DUMP-NEXT: nonnull-on positive param0
// DUMP-NEXT: function 'vec_reset':
// DUMP-NEXT: always-returns
// DUMP-NEXT: release *param0->items free when always
// DUMP-NEXT: store param0->items := null
// The summary goes to stderr and the dump to stdout, and `2>&1` joins them on
// one descriptor. The summary must land after the whole dump, never inside a
// dump line: only draining the dump stream first orders two buffered streams
// over one descriptor (LedgerOutput.cpp, printSummary). Without that the point
// they interleave at is the point some buffer happened to fill, which differs
// between libcs -- on glibc it fell inside the 'vec_grow' line above.
// DUMP: program slots:
// DUMP: weavec: program program: {{[0-9]+}} sites in 2 units:

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
  return *p;
}

int passes_maybe_null(void) {
  struct node *n = malloc(sizeof *n);
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
  // `find`'s result is `unknown` across the unit boundary (RFC 0031 §6.1;
  // in one unit, `strchr`'s interior result gives `invalid-release`): the
  // release is not proven, and `s` may be leaked where `p` is not `s`
  // (test/cases/KNOWN-DIFFERENCES.md, *Lit tests*).
  // CHECK: rfc0008-validity.c:[[@LINE+4]]:3: warning: 's' is leaked [weavec::leak]
  // LEDGER: "text": "free(p)",
  // LEDGER: "temporal": {
  // LEDGER-NEXT: "outcome": "unresolved",
  free(p);
}

// Clean: the outcome of `node_open` proves `*out` non-null; the grown vector
// is used through the place, not through a stale copy. (`node_open`'s
// allocation was never made on its zero class, RFC 0031 §6.3 `absent-on`.)
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

// CHECK: 1 warning and 2 errors generated.
