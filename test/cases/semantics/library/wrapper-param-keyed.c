// RFC 0030 §9.1: a wrapper that frees when its size parameter is zero is keyed to that argument.
// STAGE: S7
// 'xrealloc' frees 'p' and returns null when 'n' is 0, and otherwise behaves as realloc: its
// cases are 'result null and param 1 =0' and 'result nonnull'. In 'drop' the test of the
// result selects the null class and the known argument 0 satisfies the parameter test, so
// the record is unconditional and the use is a definite use-after-free. In 'grow' the null
// class with a size of 16 leaves 'q' live, so freeing it there is no double free.
#include <stdlib.h>

void *xrealloc(void *p, size_t n) {
  if (n == 0) {
    free(p);
    return NULL;
  }
  return realloc(p, n);
}

int drop(char *p) {
  if (xrealloc(p, 0) != NULL) return 0;
  return p[0]; // BUG: use-after-free definite
}

char *grow(char *q) {
  char *r = xrealloc(q, 16);
  if (r == NULL) {
    free(q);
    return NULL;
  }
  return r;
}
