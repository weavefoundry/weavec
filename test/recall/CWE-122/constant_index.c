// CWE-122: heap-based buffer overflow, a constant index one past the end.
#include "recall.h"

void bad(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  buf[10] = 'A'; // RECALL: out-of-bounds
  free(buf);
}

void good(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  buf[9] = 'A';
  free(buf);
}
