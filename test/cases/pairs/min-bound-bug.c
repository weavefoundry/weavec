// RFC 0030 §13.2 (step 4, S3 amendment): the defect needs the definition in
// another unit, so it is a definite error at link. The objects are already
// compiled then, so a link error lowered with -Wno-error cannot add a check;
// the case pins the link-time error only.
// RFC 0017: added regression pair. Both upper bounds of the helper loop determine its caller requirement.
// UNITS: Inputs/numeric-helpers.c
// FLAGS: -std=c11
#include "Inputs/numeric-helpers.h"
void run(void) {
  char *p = malloc(4);
  if (!p) return;
  fill_min(p, 5, 9); // BUG: out-of-bounds
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
