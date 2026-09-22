// RFC 0030 §8.2 (realloc): with an unknown size, the zero-size release is only possible.
// STAGE: S4
// The same code as realloc-zero-definite.c with a size the caller does not know: the null
// class releases 'p' only when the size is zero, so the record is conditional and the free
// is a possible double free, a warning. The program builds; the run's size is non-zero.
// RUN-INPUT:
#include <stdlib.h>

char *grow(char *p, size_t n) {
  char *q = realloc(p, n);
  if (!q) free(p); // BUG: double-free possible
  return q;
}

int main(int argc, char **argv) {
  char *p = malloc(4);
  (void)argv;
  if (!p) return 1;
  char *q = grow(p, (size_t)argc + 7);
  free(q);
  return 0;
}
