// CWE-401: memory leak, an allocation not freed on every path.
#include "recall.h"

void bad(void) {
  char *data = malloc(100);
  if (!data)
    return;
  memset(data, 'A', 100);
  print_bytes(data, 100); // RECALL: leak
}

int bad_early_return(int flag) {
  char *data = malloc(100);
  if (!data)
    return -1;
  if (flag)
    return 0; // RECALL: leak
  free(data);
  return 1;
}

void good(void) {
  char *data = malloc(100);
  if (!data)
    return;
  memset(data, 'A', 100);
  free(data);
}
