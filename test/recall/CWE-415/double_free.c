// CWE-415: double free, the same allocation released twice.
#include "recall.h"

void bad(void) {
  char *data = malloc(100);
  if (!data)
    return;
  free(data);
  free(data); // RECALL: double-free
}

void bad_on_path(int flag) {
  char *data = malloc(100);
  if (!data)
    return;
  if (flag)
    free(data);
  free(data); // RECALL: double-free
}

void good(int flag) {
  char *data = malloc(100);
  if (!data)
    return;
  if (flag) {
    free(data);
    return;
  }
  free(data);
}
