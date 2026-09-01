// Paths are joined conservatively: a pointer freed on any path is treated as
// possibly freed afterwards.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

void maybe_freed(int c) {
  int *p = malloc(4);
  if (c)
    free(p);
  else
    use(p); // fine: p is live on this path
  // CHECK: branches.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
}

void freed_in_loop(int n) {
  int *p = malloc(4);
  for (int i = 0; i < n; ++i)
    free(p);
  // CHECK: branches.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
}

// CHECK: 2 errors generated.
