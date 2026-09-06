// RFC 0013: entry guards on output graphs survive global remapping and sidecars.
// RUN: not %weavec --whole-program %s %S/Inputs/heap13.c -- 2>&1 | FileCheck %s
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %S/Inputs/heap13.c -o %t/library.o 2>&1 | count 0
// RUN: %weavec_cc -Wno-weavec-annotation-required -c %s -o %t/caller.o 2>&1 | count 0
// RUN: not %weavec_cc %t/library.o %t/caller.o -o %t/program 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include "Inputs/heap13.h"
void bad(void) {
  heap13_ensure();
  if (!heap13_singleton || !heap13_singleton->data) return;
  heap13_drop_child();
  heap13_ensure();
  // CHECK: rfc0013-lazy.c:[[@LINE+1]]:3: error: use of 'heap13_singleton->data' after it was freed [weavec::use-after-free]
  heap13_singleton->data[0] = 0;
  free(heap13_singleton); heap13_singleton = NULL;
}
int main(void) { return 0; }
// CHECK: 1 error generated.
