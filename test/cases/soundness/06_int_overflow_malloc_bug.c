// Heap OOB: 32-bit size multiplication wraps, then loop writes n elements.
// RUN-INPUT: 1073741825
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  unsigned n = (unsigned)strtoul(argv[1], NULL, 10);
  unsigned bytes = n * 4u;              /* wraps for n >= 2^30 */
  int *a = malloc(bytes);
  if (!a) return 1;
  for (unsigned i = 0; i < n; i++) a[i] = 0; // BUG: out-of-bounds // TRAP: index
  free(a);
  return 0;
}
