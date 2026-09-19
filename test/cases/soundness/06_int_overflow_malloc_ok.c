// CLEAN
// RUN-INPUT: 1073741825
// RUN-INPUT: 16
// ASAN
#include <stdint.h>
#include <stdlib.h>
int main(int argc, char **argv) {
  if (argc < 2) return 0;
  unsigned n = (unsigned)strtoul(argv[1], NULL, 10);
  if (n > UINT32_MAX / 4u) return 1;
  unsigned bytes = n * 4u;
  int *a = malloc(bytes);
  if (!a) return 1;
  for (unsigned i = 0; i < n; i++) a[i] = 0;
  free(a);
  return 0;
}
