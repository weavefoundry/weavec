// CWE-127: buffer under-read, a read at a negative index.
#include "recall.h"

int bad(void) {
  int data[10];
  memset(data, 0, sizeof data);
  return data[-1]; // RECALL: out-of-bounds
}

int bad_pointer(void) {
  char buf[10];
  memset(buf, 0, 10);
  char *p = buf - 2;
  return p[1]; // RECALL: out-of-bounds
}
