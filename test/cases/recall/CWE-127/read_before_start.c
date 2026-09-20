// CWE-127: buffer under-read, a read at a negative index.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

int bad(void) {
  int data[10];
  memset(data, 0, sizeof data);
  return data[-1]; // BUG: out-of-bounds // TRAP: index
}

int bad_pointer(void) {
  char buf[10];
  memset(buf, 0, 10);
  char *p = buf - 2;
  return p[1]; // BUG: out-of-bounds // TRAP: span
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  if (which == 1) bad();
  if (which == 2) bad_pointer();
  return 0;
}
