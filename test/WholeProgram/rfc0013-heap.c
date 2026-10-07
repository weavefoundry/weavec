// RFC 0013: heap state across units in the whole-program analysis.
// RUN: not %weavec --whole-program %s %S/Inputs/heap13.c -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "Inputs/heap13.h"

// The format-30 summaries (RFC 0031 *Summary format 30*): `heap13_new`'s
// result owns a fresh 4-byte block in `data`, and `heap13_wrap`'s result
// holds its argument there.

void overflow(void) {
  struct heap13_box *b = heap13_new(); if (!b) return;
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: error: 'b->data[4]' is out of bounds: index 4 of an object of 4 bytes [weavec::out-of-bounds]
  b->data[4] = 0;
  free(b->data); free(b);
}
void leak(void) {
  struct heap13_box *b = heap13_new(); if (!b) return;
  // The object engine names the leaked block by the call that made it
  // (RFC 0031 §5.8).
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: warning: result of 'heap13_new' is leaked [weavec::leak]
  free(b);
}
void alias(void) {
  char *p = malloc(4); if (!p) return;
  struct heap13_box *b = heap13_wrap(p);
  if (!b) { free(p); return; }
  free(p);
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: error: use of 'b->data' after it was freed [weavec::use-after-free]
  b->data[0] = 0;
  free(b);
}
void clean(void) {
  struct heap13_box *b = heap13_new(); if (!b) return;
  b->data[3] = 0; free(b->data); free(b);
}
int main(void) { clean(); return 0; }
// CHECK: 1 warning and 2 errors generated.
