// CWE-122: heap-based buffer overflow, the allocation counts elements as
// bytes.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

void bad(void) {
  int *data = malloc(10);
  if (!data)
    return;
  data[9] = 0; // BUG: out-of-bounds // TRAP: index
  free(data);
}

void bad_loop(void) {
  int *data = malloc(10);
  if (!data)
    return;
  for (int i = 0; i < 10; i++)
    data[i] = i; // BUG: out-of-bounds // TRAP: index
  free(data);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  if (which == 1) bad();
  if (which == 2) bad_loop();
  return 0;
}
