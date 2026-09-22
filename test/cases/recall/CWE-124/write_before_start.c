// CWE-124: buffer underwrite, a write at a negative index.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

void bad(void) {
  char buf[10];
  buf[-1] = 'A'; // BUG: out-of-bounds // TRAP: index
  print_bytes(buf, 10);
}

void bad_heap(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  buf[-1] = 'A'; // BUG: out-of-bounds // TRAP: index
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
