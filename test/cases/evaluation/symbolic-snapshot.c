// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(size_t n) {
  size_t original = n;
  char *p = malloc(n); if (!p) return;
  n = 1;
  p[original] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(4); return 0; }
