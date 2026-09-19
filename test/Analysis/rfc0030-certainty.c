// RFC 0030 §3, *Diagnostics*: a finding is definite (an error) or possible
// (a warning with the "may" wording); a possible null or bounds finding is a
// checked facet with no diagnostic; `allocation-failure` is off by default.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec -Wweavec-allocation-failure %s -- 2>&1 | FileCheck --check-prefix=ALLOC %s
// RUN: not %weavec -Wno-weavec-use-after-free %s -- 2>&1 | FileCheck --check-prefix=QUIET %s
#include <stdlib.h>

struct node {
  int v;
};

void definite(char *p) {
  free(p);
  // CHECK: rfc0030-certainty.c:[[@LINE+2]]:3: error: use of 'p' after it was freed [weavec::use-after-free]
  // CHECK: rfc0030-certainty.c:[[@LINE-2]]:3: note: freed here
  p[0] = 1;
}

// Freed on one path into the use: possible.
void possible(char *p, int c) {
  if (c)
    free(p);
  // CHECK: rfc0030-certainty.c:[[@LINE+2]]:3: warning: use of 'p' after it may have been freed [weavec::use-after-free]
  // CHECK: rfc0030-certainty.c:[[@LINE-2]]:5: note: freed here on some paths
  p[0] = 1;
}

void twice(char *p, int c) {
  if (c)
    free(p);
  // CHECK: rfc0030-certainty.c:[[@LINE+2]]:3: warning: 'p' may be freed twice [weavec::double-free]
  // CHECK: rfc0030-certainty.c:[[@LINE-2]]:5: note: previously freed here on some paths
  free(p);
}

// `realloc` moves its argument only when it succeeds: untested, possible.
void untested(char *p) {
  char *q = realloc(p, 16);
  // CHECK: rfc0030-certainty.c:[[@LINE+1]]:3: warning: use of 'p' after it may have been moved [weavec::use-after-move]
  p[0] = 1;
  free(q);
}

// A loop's first iteration frees nothing yet: the second free is possible,
// the use after it in the same iteration definite.
void loop(char *p, int n) {
  while (n--) {
    // CHECK: rfc0030-certainty.c:[[@LINE+1]]:5: warning: 'p' may be freed twice [weavec::double-free]
    free(p);
    // CHECK: rfc0030-certainty.c:[[@LINE+1]]:5: error: use of 'p' after it was freed [weavec::use-after-free]
    p[0] = 1;
  }
}

void storage(int c) {
  char buf[8];
  char *p = c ? buf : malloc(8);
  // CHECK: rfc0030-certainty.c:[[@LINE+2]]:3: warning: 'p' is released but may point to 'buf', which is not a heap object [weavec::invalid-release]
  // CHECK: rfc0030-certainty.c:[[@LINE-3]]:8: note: 'buf' is declared here
  free(p);
}

// Null: definite only.
int null(struct node *n) {
  if (n == NULL)
    // CHECK: rfc0030-certainty.c:[[@LINE+1]]:12: error: dereference of 'n', which is null [weavec::null-dereference]
    return n->v;
  return 0;
}

// An allocation's result used untested is checked, and an
// `allocation-failure` only when that is enabled.
// CHECK-NOT: allocation-failure
// ALLOC: rfc0030-certainty.c:[[@LINE+4]]:3: warning: the result of 'malloc' is used without a null test; it is null when allocation fails [weavec::allocation-failure]
// ALLOC: rfc0030-certainty.c:[[@LINE+2]]:20: note: allocated here
void unchecked(void) {
  struct node *n = malloc(sizeof *n);
  n->v = 1;
  free(n);
}

// Bounds: definite only, against an exact extent.
void bounds(int i) {
  char a[4];
  // CHECK: rfc0030-certainty.c:[[@LINE+1]]:3: error: 'a[4]' is out of bounds: index 4 of an object of 4 bytes [weavec::out-of-bounds]
  a[4] = 0;
  if (i >= 0 && i <= 4)
    a[i] = 0; // may be 4: a checked facet, no diagnostic
}

// -Wno-weavec-use-after-free drops the possible warnings; errors stay.
// QUIET-NOT: may have been freed
// QUIET: error: use of 'p' after it was freed
// QUIET-NOT: may have been freed
