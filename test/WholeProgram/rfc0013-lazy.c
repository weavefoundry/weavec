// RFC 0013: entry guards on output graphs survive global remapping across units.
// RUN: not %weavec --whole-program %s %S/Inputs/heap13.c -- 2>&1 | FileCheck %s
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
