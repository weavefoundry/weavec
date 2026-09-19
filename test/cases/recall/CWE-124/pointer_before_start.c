// CWE-124: buffer underwrite through a pointer moved before the buffer.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

void bad(void) {
  char buf[10];
  char *p = buf - 8;
  p[0] = 'A'; // BUG: out-of-bounds // TRAP: span
  print_bytes(buf, 10);
}

void bad_heap(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  char *p = buf - 1;
  *p = 'A'; // BUG: out-of-bounds // TRAP: span
  free(buf);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  if (which == 1) bad();
  if (which == 2) bad_heap();
  return 0;
}
