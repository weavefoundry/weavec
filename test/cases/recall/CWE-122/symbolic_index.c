// CWE-122: heap-based buffer overflow, `p[n]` on `n` elements.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

void bad(size_t n) {
  int *data = malloc(n * sizeof *data);
  if (!data)
    return;
  data[n] = 0; // BUG: out-of-bounds // TRAP: index
  free(data);
}

void bad_loop(size_t n) {
  int *data = malloc(n * sizeof *data);
  if (!data)
    return;
  for (size_t i = 0; i <= n; i++)
    data[i] = 0; // BUG: out-of-bounds // TRAP: index
  free(data);
}

void good(size_t n) {
  int *data = malloc(n * sizeof *data);
  if (!data)
    return;
  for (size_t i = 0; i < n; i++)
    data[i] = 0;
  free(data);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good(4);
  if (which == 1) bad(4);
  if (which == 2) bad_loop(4);
  return 0;
}
