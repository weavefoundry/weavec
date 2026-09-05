// CWE-126: buffer over-read, a constant index one past the end.
#include "recall.h"

void bad(void) {
  int data[10];
  memset(data, 0, sizeof data);
  print_int(data[10]); // RECALL: out-of-bounds
}

void bad_heap(void) {
  int *data = malloc(10 * sizeof *data);
  if (!data)
    return;
  memset(data, 0, 10 * sizeof *data);
  print_int(data[10]); // RECALL: out-of-bounds
  free(data);
}

void good(void) {
  int data[10];
  memset(data, 0, sizeof data);
  print_int(data[9]);
}
