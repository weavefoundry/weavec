// CWE-122: heap-based buffer overflow, `p[n]` on `n` elements.
#include "recall.h"

void bad(size_t n) {
  int *data = malloc(n * sizeof *data);
  if (!data)
    return;
  data[n] = 0; // RECALL: out-of-bounds
  free(data);
}

void bad_loop(size_t n) {
  int *data = malloc(n * sizeof *data);
  if (!data)
    return;
  for (size_t i = 0; i <= n; i++)
    data[i] = 0; // RECALL: out-of-bounds
  free(data);
}

void good(size_t n) {
  int *data = malloc(n * sizeof *data);
  if (!data)
    return;
  for (size_t i = 0; i < n; i++)
    data[i] = 0;
  free(data);
}
