// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(void) {
  size_t n = 4; char *p = malloc(n); if (!p) return;
  n = 8;
  p[7] = 0; // BUG: value-snapshot
  free(p);
}
