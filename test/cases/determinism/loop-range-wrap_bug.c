// RFC 0037 §4: a loop over n elements of a block whose size n * 8 wrapped
// to 8: the versioned loop's range wraps 64 bits too, so the guarded loop
// runs and stops at the first element past the block.
// RUN-INPUT: 0x2000000000000001
#include <stdint.h>
#include <stdlib.h>

__attribute__((noinline)) static void fill(uint64_t *a, uint64_t n) {
  for (uint64_t i = 0; i < n; i++)
    a[i] = 0x4141414141414141ULL; // TRAP: heap-buffer-overflow
}

int main(int argc, char **argv) {
  const uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 1;
  uint64_t *a = malloc((size_t)(n * 8));
  fill(a, n);
  free(a);
  return 0;
}
