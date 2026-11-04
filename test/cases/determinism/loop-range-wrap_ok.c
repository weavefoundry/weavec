// The twin of loop-range-wrap_bug.c: a size that does not wrap.
// RUN-INPUT: 1000
// CLEAN
#include <stdint.h>
#include <stdlib.h>

__attribute__((noinline)) static void fill(uint64_t *a, uint64_t n) {
  for (uint64_t i = 0; i < n; i++)
    a[i] = 0x4141414141414141ULL;
}

int main(int argc, char **argv) {
  const uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
  uint64_t *a = malloc((size_t)(n * 8));
  fill(a, n);
  free(a);
  return 0;
}
