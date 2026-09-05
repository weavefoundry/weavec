// CWE-122: heap-based buffer overflow, the allocation counts elements as
// bytes.
#include "recall.h"

void bad(void) {
  int *data = malloc(10);
  if (!data)
    return;
  data[9] = 0; // RECALL: out-of-bounds
  free(data);
}

void bad_loop(void) {
  int *data = malloc(10);
  if (!data)
    return;
  for (int i = 0; i < 10; i++)
    data[i] = i; // RECALL: out-of-bounds
  free(data);
}
