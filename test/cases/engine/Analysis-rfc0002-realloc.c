// Engine pin converted from test/Analysis/rfc0002-realloc.c; markers are the v0.10.0 golden diagnostics.
// RFC 0002: `realloc` consumes its argument, but the old pointer is valid
// again on the path where the result is null.
#include "Inputs/prelude.h"

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
  free(p); // BUG: use-after-move
  use(q);
  return 0; // BUG: leak
}

void result_overwritten(char *p) {
  char *q = realloc(p, 8);
  q = malloc(2); // BUG: leak
  if (q == NULL) // BUG: leak
    free(p); // BUG: use-after-move
}
