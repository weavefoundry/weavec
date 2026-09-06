// RFC 0013: swap inputs and consumption of extracted old values cross sidecars.
// RUN: not %weavec --whole-program %s %S/Inputs/heap13.c -- 2>&1 | FileCheck %s
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/heap13.c -o %t/library.o 2>&1 | count 0
// RUN: %weavec_cc -Wno-weavec-annotation-required -c %s -o %t/caller.o 2>&1 | count 0
// RUN: not %weavec_cc %t/library.o %t/caller.o -o %t/program 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "Inputs/heap13.h"

void good(void) {
  struct heap13_box b = {malloc(8)};
  heap13_reset(&b);
  if (b.data) b.data[3] = 0;
  free(b.data);
}
void overflow(void) {
  struct heap13_box b = {malloc(8)};
  heap13_reset(&b);
  // CHECK: rfc0013-swap.c:[[@LINE+1]]:15: error: 'b.data[4]' is out of bounds: index 4 of an object of 4 bytes [weavec::out-of-bounds]
  if (b.data) b.data[4] = 0;
  free(b.data);
}
void stale(void) {
  struct heap13_box b = {malloc(8)}; if (!b.data) return;
  char *old = b.data;
  heap13_reset(&b);
  // CHECK: rfc0013-swap.c:[[@LINE+1]]:3: error: use of 'old' after it was freed [weavec::use-after-free]
  old[0] = 0;
  free(b.data);
}
int main(void) { return 0; }
// CHECK: 2 errors generated.
