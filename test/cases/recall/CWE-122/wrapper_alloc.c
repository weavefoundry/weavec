// CWE-122: heap-based buffer overflow through an allocation wrapper and a
// callee, the extent and the requirement both crossing a call.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

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
  put_eight(buf); // BUG: out-of-bounds // TRAP: len
  free(buf);
}

void bad_direct(void) {
  char *buf = xmalloc(4);
  buf[4] = 0; // BUG: out-of-bounds // TRAP: index
  free(buf);
}

void good(void) {
  char *buf = xmalloc(8);
  put_eight(buf);
  free(buf);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good();
  if (which == 1) bad();
  if (which == 2) bad_direct();
  return 0;
}
