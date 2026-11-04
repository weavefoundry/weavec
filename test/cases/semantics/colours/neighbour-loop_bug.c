// RFC 0037 §8.5: a loop from an attacker's offset that lies wholly in a
// live neighbour: its versioned copy runs only when the range starts in the
// root's colour, so the guarded loop runs and stops.
// RUN-INPUT: 1
#include <stdlib.h>

__attribute__((noinline)) static void fill(int *a, long k, long n) {
  for (long i = 0; i < n; i++)
    a[k + i] = 7; // TRAP: heap-buffer-overflow
}

int main(int argc, char **argv) {
  int *a = malloc(64 * sizeof(int));
  int *b = malloc(64 * sizeof(int));
  fill(a, (b - a) * atol(argv[argc - 1]), 32);
  return b[0];
}
