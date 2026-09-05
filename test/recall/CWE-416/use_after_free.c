// CWE-416: use after free, a read and a write through a freed pointer.
#include "recall.h"

void bad_read(void) {
  char *data = malloc(100);
  if (!data)
    return;
  memset(data, 'A', 100);
  free(data);
  print_bytes(data, 100); // RECALL: use-after-free
}

void bad_write(void) {
  char *data = malloc(100);
  if (!data)
    return;
  free(data);
  data[0] = 'A'; // RECALL: use-after-free
}

void good(void) {
  char *data = malloc(100);
  if (!data)
    return;
  memset(data, 'A', 100);
  print_bytes(data, 100);
  free(data);
}
