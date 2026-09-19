// CWE-122: heap-based buffer overflow, a constant index one past the end.
#include "../recall.h"

void bad(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  buf[10] = 'A'; // BUG: out-of-bounds // TRAP: index
  free(buf);
}

void good(void) {
  char *buf = malloc(10);
  if (!buf)
    return;
  buf[9] = 'A';
  free(buf);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) {
  good();
  bad();
  return 0;
}
