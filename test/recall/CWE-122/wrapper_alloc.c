// CWE-122: heap-based buffer overflow through an allocation wrapper and a
// callee, the extent and the requirement both crossing a call.
#include "recall.h"

static char *xmalloc(size_t n) {
  char *p = malloc(n);
  if (!p)
    __builtin_trap();
  return p;
}

static void put_eight(char *b) {
  for (int i = 0; i < 8; i++)
    b[i] = 'A';
}

void bad(void) {
  char *buf = xmalloc(4);
  put_eight(buf); // RECALL: out-of-bounds
  free(buf);
}

void bad_direct(void) {
  char *buf = xmalloc(4);
  buf[4] = 0; // RECALL: out-of-bounds
  free(buf);
}

void good(void) {
  char *buf = xmalloc(8);
  put_eight(buf);
  free(buf);
}
