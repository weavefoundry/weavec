// RFC 0017: added regression pair. The repeated symbolic C product denotes the actual allocated byte count.
#include "../../Inputs/prelude.h"
void run(size_t rows, size_t cols) {
  size_t bytes = rows * cols;
  if (!bytes) return;
  char *p = malloc(rows * cols);
  if (!p) return;
  p[rows * cols] = 0; // BUG: product
  free(p);
}
