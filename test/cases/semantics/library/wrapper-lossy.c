// RFC 0030 §9.1: an effect that lost a conjunct is lossy and only ever gives a possible warning.
// STAGE: S7
// 'grow' frees 'ptr' only when 'm' is non-null and the local 'moved' is set. The conjunct on
// 'moved' is neither a result class nor a parameter zero-test, so it is dropped and the
// effect 'ptr freed when the result is non-null' carries the lossy bit. The caller's result
// test selects the class, but a record made from a lossy effect stays conditional: the use
// of 'p' is a possible use-after-free, never a definite one. The run with an argument takes
// the freeing path, which ASan reports.
// RUN-INPUT: x
// ASAN
#include <stdlib.h>
#include <string.h>

void *grow(void *ptr, size_t old, size_t n) {
  int moved = n > 64;
  void *m = malloc(n);
  if (m && moved) {
    memcpy(m, ptr, old);
    free(ptr);
  }
  return m;
}

int main(int argc, char **argv) {
  char *p = malloc(8);
  (void)argv;
  if (!p) return 1;
  memset(p, 'a', 8);
  char *q = grow(p, 8, argc > 1 ? 128 : 16);
  if (q == NULL) {
    free(p);
    return 1;
  }
  int c = p[0]; // BUG: use-after-free possible
  free(q);
  return c == 'a' ? 0 : 2;
}
