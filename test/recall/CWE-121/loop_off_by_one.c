// CWE-121: stack-based buffer overflow, a counted loop that runs one too far.
#include "recall.h"

void bad(void) {
  int data[10];
  for (int i = 0; i <= 10; i++)
    data[i] = i; // RECALL: out-of-bounds
  print_int(data[0]);
}

void good(void) {
  int data[10];
  for (int i = 0; i < 10; i++)
    data[i] = i;
  print_int(data[0]);
}
