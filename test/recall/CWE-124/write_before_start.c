// CWE-124: buffer underwrite, a write at a negative index.
#include "recall.h"

void bad(void) {
  char buf[10];
  buf[-1] = 'A'; // RECALL: out-of-bounds
  print_bytes(buf, 10);
}

void bad_heap(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  buf[-1] = 'A'; // RECALL: out-of-bounds
  free(buf);
}
