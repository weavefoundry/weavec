// CWE-122: heap-based buffer overflow through memcpy of the source's size.
#include "recall.h"

void bad(void) {
  char src[100];
  char *dst = malloc(50);
  if (!dst)
    return;
  memset(src, 'A', 100);
  memcpy(dst, src, 100); // RECALL: out-of-bounds
  free(dst);
}

void bad_memset(void) {
  int *data = malloc(10 * sizeof *data);
  if (!data)
    return;
  memset(data, 0, 20 * sizeof *data); // RECALL: out-of-bounds
  free(data);
}
