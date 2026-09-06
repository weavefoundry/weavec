// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(size_t n) {
  size_t original = n;
  char *p = malloc(n); if (!p) return;
  n = 1;
  p[original] = 0; // BUG: symbolic-snapshot
  free(p);
}
