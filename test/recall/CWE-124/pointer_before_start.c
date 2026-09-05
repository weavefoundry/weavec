// CWE-124: buffer underwrite through a pointer moved before the buffer.
#include "recall.h"

void bad(void) {
  char buf[10];
  char *p = buf - 8;
  p[0] = 'A'; // RECALL: out-of-bounds
  print_bytes(buf, 10);
}

void bad_heap(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  char *p = buf - 1;
  *p = 'A'; // RECALL: out-of-bounds
  free(buf);
}
