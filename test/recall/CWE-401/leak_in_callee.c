// CWE-401: memory leak, a wrapper's fresh result is dropped by the caller.
#include "recall.h"

static char *make(size_t n) {
  char *p = malloc(n);
  if (p)
    memset(p, 0, n);
  return p;
}

void bad(void) {
  make(10); // RECALL: leak
}

void bad_struct(void) {
  struct holder { char *buf; } h;
  h.buf = make(10);
  if (!h.buf)
    return;
  h.buf = NULL; // RECALL: leak
}

void good(void) {
  char *p = make(10);
  if (!p)
    return;
  free(p);
}
