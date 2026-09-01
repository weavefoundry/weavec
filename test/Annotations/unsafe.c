// WEAVEC_UNSAFE opts a function or a block out of checking.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

WEAVEC_UNSAFE void whole_function(int *p) {
  free(p);
  free(p); // not reported
}

void unsafe_block(int *p) {
  free(p);
  WEAVEC_UNSAFE {
    use(p); // not reported
  }
  // CHECK: unsafe.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
}

// CHECK: 1 error generated.
