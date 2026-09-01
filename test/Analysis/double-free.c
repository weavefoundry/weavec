// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

void double_free(int *p) {
  free(p);
  // CHECK: double-free.c:[[@LINE+1]]:3: error: 'p' is freed twice [weavec::double-free]
  free(p);
  // CHECK: double-free.c:[[@LINE-3]]:3: note: previously freed here
}

// CHECK: 1 error generated.
