// RFC 0016: one source operation versus several paths, and ordered effects.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

static void after(char *a, char *b) {
  free(a);
  // CHECK: rfc0016-composition.c:[[@LINE+1]]:4: error: use of 'b' after it was freed [weavec::use-after-free]
  *b = 1;
}
static void twice(char *a, char *b) {
  free(a);
  // CHECK: rfc0016-composition.c:[[@LINE+1]]:3: error: 'b' is freed twice [weavec::double-free]
  free(b);
}
static void after_output(char **a, char **b) {
  free(*a);
  // CHECK: rfc0016-composition.c:[[@LINE+1]]:4: error: use of '*b' after it was freed [weavec::use-after-free]
  **b = 1;
}
static void forward(char *a, char *b) { after(a, b); }
void bad(void) {
  char *p = malloc(4);
  if (!p) return;
  forward(p, p);
  p = malloc(4);
  if (!p) return;
  twice(p, p);
  p = malloc(4);
  if (!p) return;
  after_output(&p, &p);
}
// CHECK: 3 errors generated.
