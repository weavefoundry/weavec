// RFC 0017: added regression pair. A separate helper publishes a narrowed value through a wider output cell.
// CLEAN
// UNITS: Inputs/numeric-helpers.c
// FLAGS: -std=c11
#include "Inputs/numeric-helpers.h"
void run(unsigned n) {
  if (n != 257) return;
  size_t bytes;
  narrow_count_out(n, &bytes);
  char *p = malloc(bytes);
  if (!p) return;
  p[0] = 0;
  free(p);
}
