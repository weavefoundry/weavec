// CWE-121: stack-based buffer overflow, a constant index one past the end.
#include "recall.h"

void bad(void) {
  char buf[10];
  buf[10] = 'A'; // RECALL: out-of-bounds
}

void good(void) {
  char buf[10];
  buf[9] = 'A';
  print_bytes(buf, 10);
}
