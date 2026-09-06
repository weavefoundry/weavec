// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(size_t rows, size_t cols) {
  char *p = malloc(rows * cols); if (!p) return;
  p[rows * cols] = 0; // BUG: product-known-miss
  free(p);
}
