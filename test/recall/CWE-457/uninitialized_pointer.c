// CWE-457: use of an uninitialised pointer variable.
#include "recall.h"

void bad(void) {
  char *data;
  data[0] = 'A'; // RECALL: use-of-uninitialized
}

void bad_on_path(int flag) {
  char buf[10];
  char *data;
  if (flag)
    data = buf;
  print_line(data); // RECALL: use-of-uninitialized
}

void good(int flag) {
  char *data = NULL;
  if (flag)
    data = malloc(10);
  if (!data)
    return;
  data[0] = 'A';
  free(data);
}
