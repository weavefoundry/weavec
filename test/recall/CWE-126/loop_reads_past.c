// CWE-126: buffer over-read, a loop that reads one too many.
#include "recall.h"

int bad(void) {
  int data[10];
  int sum = 0;
  memset(data, 0, sizeof data);
  for (int i = 0; i <= 10; i++)
    sum += data[i]; // RECALL: out-of-bounds
  return sum;
}

void bad_memcpy_source(void) {
  char small[8];
  char big[16];
  memset(small, 0, 8);
  memcpy(big, small, 16); // RECALL: out-of-bounds
  print_bytes(big, 16);
}
