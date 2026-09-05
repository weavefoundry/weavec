// CWE-121: stack-based buffer overflow through a pointer stepped past the end.
#include "recall.h"

void bad(void) {
  char buf[8];
  char *p = buf + 4;
  p[4] = 0; // RECALL: out-of-bounds
  print_bytes(buf, 8);
}

void good(void) {
  char buf[8];
  char *p = buf + 4;
  p[3] = 0;
  print_bytes(buf, 8);
}
