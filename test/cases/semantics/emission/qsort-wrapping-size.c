// RFC 0030 §10.2: a wrapping 'nmemb * size' for qsort saturates and traps instead of passing.
// STAGE: S5
// qsort's row requires bytes(a1*a2) behind the base. The need term is computed by the term
// helpers, which saturate to the maximum on overflow, so the len check fails where C's
// wrapping product (4 here) would pass and let qsort run past 'xs'. The run passes
// n == 2^62 + 1.
// RUN-INPUT: 4611686018427387905
// ASAN
#include <stdlib.h>

static int cmp(const void *a, const void *b) {
  int x = *(const int *)a;
  int y = *(const int *)b;
  return (x > y) - (x < y);
}

int main(int argc, char **argv) {
  int xs[4] = {4, 3, 2, 1};
  size_t n = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 4;
  qsort(xs, n, sizeof xs[0], cmp); // BUG: out-of-bounds // TRAP: len
  return xs[0];
}
