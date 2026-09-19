// RFC 0030 §13.2 (step 4, S3 amendment): the defect needs the definition in
// another unit, so it is a definite error at link. The objects are already
// compiled then, so a link error lowered with -Wno-error cannot add a check;
// the case pins the link-time error only.
// RFC 0017: added regression pair. A separate helper returns the narrowed count one, not its incoming 257.
// UNITS: Inputs/numeric-helpers.c
// FLAGS: -std=c11
#include "Inputs/numeric-helpers.h"
void run(unsigned n) {
  if (n != 257) return;
  char *p = malloc(narrow_count(n));
  if (!p) return;
  p[1] = 0; // BUG: out-of-bounds
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(257); return 0; }
