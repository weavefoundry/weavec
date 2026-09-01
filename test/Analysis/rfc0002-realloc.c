// RFC 0002: `realloc` consumes its argument, but the old pointer is valid
// again on the path where the result is null.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

int grow(char **buf) {
  char *p = *buf;
  char *q = realloc(p, 16);
  if (q == NULL) {
    free(p); // fine: realloc failed, p is still ours
    return -1;
  }
  *buf = q;
  return 0;
}

void grow_in_place(void) {
  char *p = malloc(4);
  p = realloc(p, 8);
  if (!p)
    return;
  free(p);
}

void grow_in_loop(char *p, int n) {
  for (int i = 0; i < n; ++i) {
    char *q = realloc(p, 8);
    if (!q) {
      free(p);
      return;
    }
    p = q;
  }
  free(p);
}

int no_null_test(char *p) {
  char *q = realloc(p, 16);
  // CHECK: rfc0002-realloc.c:[[@LINE+1]]:3: error: use of 'p' after it was moved [weavec::use-after-move]
  free(p);
  // CHECK: rfc0002-realloc.c:[[@LINE-3]]:13: note: moved here
  use(q);
  return 0;
}

void result_overwritten(char *p) {
  char *q = realloc(p, 8);
  q = malloc(2);
  if (q == NULL)
    // CHECK: rfc0002-realloc.c:[[@LINE+1]]:5: error: use of 'p' after it was moved [weavec::use-after-move]
    free(p);
}

// CHECK: 2 errors generated.
