// RFC 0017: added regression pair. Both upper bounds of the helper loop determine its caller requirement.
// UNITS: Inputs/numeric-helpers.c
// FLAGS: -std=c11
#include "Inputs/numeric-helpers.h"
void run(void) {
  char *p = malloc(4);
  if (!p) return;
  fill_min(p, 5, 9); // BUG: out-of-bounds // TRAP: len
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
