// RFC 0030 §13.2 (step 4, S3 amendment): the defect needs the definition in
// another unit, so it is a definite error at link. The objects are already
// compiled then, so a link error lowered with -Wno-error cannot add a check;
// the case pins the link-time error only.
// RFC 0013: fixed evaluation case.
// UNITS: Inputs/heap.c
#include "Inputs/heap.h"
void run(void) {
  struct box *b = box_new();
  if (!b) return;
  b->data[4] = 0; // BUG: out-of-bounds
  free(b->data);
  free(b);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
