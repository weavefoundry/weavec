// RFC 0006, *Outcome-conditional summaries*: a callee that consumes its
// argument only on the paths returning some class of value is summarised
// per class, and a caller's test of the result retracts the consumption on
// the edge where it did not happen. `realloc` is the library instance: it
// moves its argument on the non-null class and, when the size is zero, on
// the null class as well (RFC 0030 §8.2).
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

struct node {
  int v;
};

// Consumes `n` only when it returns 0.
static int try_take(struct node *n, int c) {
  if (c) {
    free(n);
    return 0;
  }
  return -1;
}
// The release happens only when `c` is non-zero: each result class carries
// the parameter test its exits pass, and the release is keyed by the class
// (RFC 0031 §6.1 and *Implementation amendments*, *Pending cases*).
// DUMP-LABEL: function 'try_take':
// DUMP: result int [-1, -1] when negative and param 1 =0
// DUMP-NEXT: result int [0, 0] when zero and param 1 !=0
// DUMP-NEXT: release *param0 free when result zero

// Consumes `p` only when it returns non-null (a `realloc` wrapper).
static char *grow(char *p, size_t n) {
  char *q = realloc(p, n);
  if (!q)
    return NULL;
  return q;
}
// `realloc` moves its argument into the result on the nonnull class; on the
// null class it keeps it (the zero-initialisation wrapper never asks for
// zero bytes, RFC 0031 *Implementation amendments*).
// DUMP-LABEL: function 'grow':
// DUMP: result null when null
// DUMP-NEXT: result fresh#0 free extent param1 zeroed when nonnull
// DUMP-NEXT: move *param0 free when result nonnull
// DUMP-LABEL: function 'guarded':
// DUMP: release *param0 free when always

// Clean: the test selects the class that did not consume.
void guarded(struct node *n, int c) {
  int rc = try_take(n, c);
  if (rc != 0)
    free(n);
}

void guarded_direct(struct node *n, int c) {
  if (try_take(n, c) < 0)
    free(n);
}

void guarded_in_condition(struct node *n, int c) {
  int rc;
  if ((rc = try_take(n, c)) == 0)
    return;
  n->v = 1;
  free(n);
}

void grown(char *p) {
  char *q = grow(p, 16);
  if (q == NULL) {
    free(p); // `grow` failed: `p` is still ours
    return;
  }
  free(q);
}

void realloc_null_edge(char **buf) {
  char *p = *buf;
  char *q = realloc(p, 16);
  if (q == NULL) {
    free(p);
    return;
  }
  *buf = q;
}

// A callee that reallocates a path below its argument and returns it (Lua's
// `resizearray`): the result is the resource itself, not a dangling copy of
// what was consumed (RFC 0006, *Interaction with existing RFCs*).
struct table {
  char *array;
  size_t n;
};
static char *resize(struct table *t, size_t n) {
  if (n == t->n)
    return t->array;
  return realloc(t->array, n);
}
// The result is the fresh block, null, or `t->array` itself; the move of
// `t->array` is keyed by the result class, and possible because the
// `n == t->n` exit returns the array unmoved.
// DUMP-LABEL: function 'resize':
// DUMP: result fresh#0 free extent param1 zeroed when nonnull and param 0 !=0
// DUMP-NEXT: result path param0->array when null nonnull and param 0 !=0
// DUMP-NEXT: move *param0->array free may when result nonnull

void resized(struct table *t, size_t n) {
  char *na = resize(t, n);
  if (na == NULL)
    return;
  t->array = na;
  use(t->array);
}

// Reported: the selected class consumed, or nothing was tested.
void wrong_branch(struct node *n, int c) {
  int rc = try_take(n, c);
  // `try_take` returns 0 only after freeing `n`, so on this edge the release
  // is certain (RFC 0031 §6.3, the result class selects the effect).
  if (rc == 0)
    // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:9: error: use of 'n' after it was freed [weavec::use-after-free]
    use(n);
}

void untested(struct node *n, int c) {
  try_take(n, c);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: warning: use of 'n' after it may have been freed [weavec::use-after-free]
  n->v = 1;
}

void realloc_untested(char *p) {
  char *q = realloc(p, 16);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: warning: use of 'p' after it may have been moved [weavec::use-after-move]
  free(p);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: warning: 'q' is leaked [weavec::leak]
  use(q);
}

void result_overwritten(char *p) {
  char *q = realloc(p, 8);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: warning: 'q' is leaked: it is overwritten without being released [weavec::leak]
  q = malloc(2);
  // Reported at the branch that takes the leaking path (RFC 0031
  // *Implementation amendments*, *Leaks on some paths*).
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:7: warning: 'q' is leaked [weavec::leak]
  if (q == NULL)
    // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:5: warning: use of 'p' after it may have been moved [weavec::use-after-move]
    free(p);
}

// CHECK: 6 warnings and 1 error generated.
