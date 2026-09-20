// CWE-122: heap-based buffer overflow through memcpy of the source's size.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

void bad(void) {
  char src[100];
  char *dst = malloc(50);
  if (!dst)
    return;
  memset(src, 'A', 100);
  memcpy(dst, src, 100); // BUG: out-of-bounds // TRAP: len
  free(dst);
}

void bad_memset(void) {
  int *data = malloc(10 * sizeof *data);
  if (!data)
    return;
  memset(data, 0, 20 * sizeof *data); // BUG: out-of-bounds // TRAP: len
  free(data);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  if (which == 1) bad();
  if (which == 2) bad_memset();
  return 0;
}
