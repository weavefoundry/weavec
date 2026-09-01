// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

void use_after_free(void) {
  int *p = malloc(sizeof(int));
  free(p);
  // CHECK: use-after-free.c:[[@LINE+1]]:4: error: use of 'p' after it was freed [weavec::use-after-free]
  *p = 1;
  // CHECK: use-after-free.c:[[@LINE-3]]:3: note: freed here
}

void use_via_call(void) {
  int *p = malloc(sizeof(int));
  free(p);
  // CHECK: use-after-free.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
}

// CHECK: 2 errors generated.
