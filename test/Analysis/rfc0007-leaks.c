// RFC 0007, *Leaks*: an owned resource whose every holder goes out of reach
// without being released, moved or escaped is reported once, at the point
// it is lost (a return, a scope end, an overwrite, a discarded call).
// RUN: %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

char *strdup(const char *s);

struct buf {
  char *data;
  size_t n;
};

// The RFC's snippets that must be reported.

int leak_path(int c) {
  char *p = malloc(8);
  if (c)
    // CHECK: rfc0007-leaks.c:[[@LINE+2]]:5: warning: 'p' is leaked [weavec::leak]
    // CHECK: rfc0007-leaks.c:[[@LINE-3]]:13: note: allocated here
    return -1;
  free(p);
  return 0;
}

void overwrite(void) {
  char *p = malloc(8);
  // CHECK: rfc0007-leaks.c:[[@LINE+1]]:3: warning: 'p' is leaked: it is overwritten without being released [weavec::leak]
  p = malloc(16);
  free(p);
}

void owned_param(char *WEAVEC_OWNED p) {
  // CHECK: rfc0007-leaks.c:[[@LINE+2]]:3: warning: 'p' is leaked [weavec::leak]
  // CHECK: rfc0007-leaks.c:[[@LINE-2]]:37: note: 'p' is declared WEAVEC_OWNED here
  use(p);
}

void discarded(const char *s) {
  // CHECK: rfc0007-leaks.c:[[@LINE+1]]:3: warning: result of 'strdup' is leaked [weavec::leak]
  strdup(s);
}

// A value that is never read is lost right after it is stored.
int never_used(int c) {
  char *p = malloc(8);
  // CHECK: rfc0007-leaks.c:[[@LINE+1]]:7: warning: 'p' is leaked [weavec::leak]
  if (c)
    return 1;
  return 2;
}

// The record travels with copies: the leak is reported for the holder that
// dies last, and there is no second report for `p`.
void copies(void) {
  char *p = malloc(8);
  char *q = p;
  // CHECK: rfc0007-leaks.c:[[@LINE+1]]:3: warning: 'q' is leaked [weavec::leak]
  use(q);
}

// Overwriting an owned global loses the old value for this function.
static char *global;
void global_overwrite(void) {
  global = malloc(8);
  // CHECK: rfc0007-leaks.c:[[@LINE+1]]:3: warning: 'global' is leaked: it is overwritten without being released [weavec::leak]
  global = malloc(16);
}

// A field the function itself made owned is checked on overwrite.
void field_overwrite(struct buf *b) {
  b->data = malloc(8);
  // CHECK: rfc0007-leaks.c:[[@LINE+1]]:3: warning: 'b->data' is leaked: it is overwritten without being released [weavec::leak]
  b->data = malloc(16);
}

// Once a merge-point false positive: `p` may own at the second `if`, but the
// resource is held under the guard `c != 0`, which the early return's edge
// refutes (RFC 0009, *Refuting guards in the state*).
int merged(int c) {
  char *p = NULL;
  if (c)
    p = malloc(8);
  if (!c)
    return -1;
  free(p);
  return 0;
}

// CHECK: 8 warnings generated.
