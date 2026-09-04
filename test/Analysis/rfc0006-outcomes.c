// RFC 0006, *Outcome-conditional summaries*: a callee that consumes its
// argument only on the paths returning some class of value is summarised
// per class, and a caller's test of the result retracts the consumption on
// the edge where it did not happen. `realloc` is the built-in instance.
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
// DUMP: function 'try_take':
// DUMP: summary: n: freed(free); stores{} returns{} outcome zero{n: freed(free)} outcome negative{}

// Consumes `p` only when it returns non-null (a `realloc` wrapper).
static char *grow(char *p, size_t n) {
  char *q = realloc(p, n);
  if (!q)
    return NULL;
  return q;
}
// DUMP: function 'grow':
// DUMP: summary: p: moved(free); stores{} returns{fresh(free), null} outcome null{} outcome nonnull{p: moved(free)}
// DUMP: function 'guarded':
// DUMP: summary: n: freed(free); stores{} returns{}

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
// DUMP: function 'resize':
// DUMP: summary: t->array: read|moved(free); t->n: read; stores{} returns{fresh(free), copy t->array, null} requires{t} outcome null{} outcome nonnull{t->array: moved(free)}

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
  if (rc == 0)
    // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:9: error: use of 'n' after it was freed [weavec::use-after-free]
    use(n);
}

void untested(struct node *n, int c) {
  try_take(n, c);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: error: use of 'n' after it was freed [weavec::use-after-free]
  n->v = 1;
}

void realloc_untested(char *p) {
  char *q = realloc(p, 16);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: error: use of 'p' after it was moved [weavec::use-after-move]
  free(p);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: warning: 'q' is leaked [weavec::leak]
  use(q);
}

void result_overwritten(char *p) {
  char *q = realloc(p, 8);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: warning: 'q' is leaked: it is overwritten without being released [weavec::leak]
  q = malloc(2);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:3: warning: 'q' is leaked [weavec::leak]
  if (q == NULL)
    // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:5: error: use of 'p' after it was moved [weavec::use-after-move]
    free(p);
}

// CHECK: 3 warnings and 4 errors generated.
