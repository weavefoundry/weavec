// CWE-476: NULL pointer dereference, an allocation used without a check and
// a pointer set to NULL then dereferenced.
#include "recall.h"

void bad_unchecked_malloc(void) {
  char *data = malloc(100);
  data[0] = 'A'; // RECALL: null-dereference
  free(data);
}

void bad_assigned_null(void) {
  int *p = NULL;
  *p = 1; // RECALL: null-dereference
}

void bad_after_test(int *p) {
  if (p == NULL)
    print_int(*p); // RECALL: null-dereference
}

void good(void) {
  char *data = malloc(100);
  if (!data)
    return;
  data[0] = 'A';
  free(data);
}
