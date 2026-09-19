// RFC 0013: fixed evaluation case.
// Listed as a known miss (required: false) in the v0.10.0 manifest; v0.10.0 reports it.
#include "Inputs/heap.h"
void run(size_t rows, size_t cols) {
  char *p = malloc(rows * cols); if (!p) return;
  p[rows * cols] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(2, 3); return 0; }
