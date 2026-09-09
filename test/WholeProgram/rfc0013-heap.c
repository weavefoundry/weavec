// RFC 0013: tool whole-program analysis and the compiler's link step agree.
// RUN: not %weavec --whole-program %s %S/Inputs/heap13.c -- 2>&1 | FileCheck %s
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/heap13.c -o %t/library.o 2>&1 | count 0
// RUN: %weavec_cc -Wno-weavec-annotation-required -c %s -o %t/caller.o 2>&1 | count 0
// RUN: FileCheck --check-prefix=SIDECAR %s < %t/library.o.weavec
// RUN: not %weavec_cc %t/library.o %t/caller.o -o %t/program 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "Inputs/heap13.h"

// SIDECAR: weavec-summaries 16
// SIDECAR: heap result complete
// SIDECAR-NEXT: heap-field result at result *.data fresh(free) extent 4
// SIDECAR: heap-field result at result *.data copy param 0

void overflow(void) {
  struct heap13_box *b = heap13_new(); if (!b) return;
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: error: 'b->data[4]' is out of bounds: index 4 of an object of 4 bytes [weavec::out-of-bounds]
  b->data[4] = 0;
  free(b->data); free(b);
}
void leak(void) {
  struct heap13_box *b = heap13_new(); if (!b) return;
  // CHECK: rfc0013-heap.c:[[@LINE+1]]:3: warning: 'b->data' is leaked when 'b' is freed [weavec::leak]
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
