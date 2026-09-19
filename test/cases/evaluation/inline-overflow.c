// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(void) {
  struct box *b = malloc(sizeof *b);
  if (!b) return;
  b->data = malloc(4);
  if (!b->data) { free(b); return; }
  b->data[4] = 0; // BUG: out-of-bounds // TRAP: index
  free(b->data);
  free(b);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
