// RFC 0012, *`WEAVEC_ASSUME`*: the argument holds from the call on, as on
// the true edge of `if (arg)`; an assumption the facts contradict ends the
// path; the annotation belongs to the header's function alone.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
//
// With the analysis off the macro is an unevaluated no-op that still counts
// as a use of its operands: no warnings from Clang.
// RUN: %weavec_cc -fno-weavec -fsyntax-only -Wall -Wextra -Wno-unused-parameter -I%resource_dir %s 2>&1 | count 0
#include "../Inputs/prelude.h"
#include <weavec.h>

struct pair {
  char *p;
  size_t len;
  size_t cap;
};

// The invariant `len < cap` is not visible here; the assumption states it.
void put(struct pair *b, char c) {
  WEAVEC_ASSUME(b->len < b->cap);
  char *d = malloc(b->cap);
  if (!d)
    return;
  d[b->len] = c;
  // CHECK: rfc0012-assume.c:[[@LINE+1]]:3: error: 'd[b->cap]' is out of bounds: 'b->cap' is the number of elements of 'd' [weavec::out-of-bounds]
  d[b->cap] = c;
  free(d);
}

// Without it the access is undecided and nothing is reported.
void put_unassumed(struct pair *b, char c) {
  char *d = malloc(b->cap);
  if (!d)
    return;
  d[b->len] = c;
  free(d);
}

// A pointer fact.
void nonnull(int *p) {
  WEAVEC_ASSUME(p != NULL);
  *p = 1;
}

// A contradicted assumption ends the path: nothing below it runs.
void contradiction(int *p) {
  if (p)
    return;
  WEAVEC_ASSUME(p != NULL);
  *p = 1;
}

void without(int *p) {
  if (p)
    return;
  // CHECK: rfc0012-assume.c:[[@LINE+1]]:4: error: dereference of 'p', which is null [weavec::null-dereference]
  *p = 1;
}

// CHECK: rfc0012-assume.c:[[@LINE+1]]:40: warning: 'weavec.assume' is not an annotation for 'mine' [weavec::invalid-annotation]
WEAVEC_ANNOTATE_("weavec.assume") void mine(int c) { (void)c; }
