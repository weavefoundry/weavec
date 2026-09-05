// CWE-121: stack-based buffer overflow, a loop bounded by the wrong constant
// (the source's size rather than the destination's).
#include "recall.h"

void bad(void) {
  int src[100];
  int dst[50];
  for (int i = 0; i < 100; i++)
    dst[i] = src[i]; // RECALL: out-of-bounds
  print_int(dst[0]);
}
