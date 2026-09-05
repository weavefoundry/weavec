// CWE-121: stack-based buffer overflow through memcpy with the source's size.
#include "recall.h"

void bad(void) {
  char src[100];
  char dst[50];
  memset(src, 'A', 100);
  memcpy(dst, src, 100); // RECALL: out-of-bounds
  print_bytes(dst, 50);
}

void good(void) {
  char src[100];
  char dst[50];
  memset(src, 'A', 100);
  memcpy(dst, src, 50);
  print_bytes(dst, 50);
}
