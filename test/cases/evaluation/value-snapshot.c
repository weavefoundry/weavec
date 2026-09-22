// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(void) {
  size_t n = 4; char *p = malloc(n); if (!p) return;
  n = 8;
  p[7] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
