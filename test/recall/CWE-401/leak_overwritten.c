// CWE-401: memory leak, the only pointer to an allocation is overwritten.
#include "recall.h"

void bad(void) {
  char *data = malloc(100);
  if (!data)
    return;
  data = malloc(200); // RECALL: leak
  if (!data)
    return;
  free(data);
}

void bad_swapped(void) {
  char *a = malloc(100);
  char *b = malloc(200);
  if (!a || !b) {
    free(a);
    free(b);
    return;
  }
  a = b; // RECALL: leak
  free(a);
}
