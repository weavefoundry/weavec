// RFC 0013: fixed evaluation case.
// Listed as a known miss (required: false) in the v0.10.0 manifest; v0.10.0 reports it.
#include "Inputs/heap.h"
void run(size_t n) {
  if (!n) return;
  char local[n];
  local[n] = 0; // BUG: out-of-bounds // TRAP: index
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(4); return 0; }
