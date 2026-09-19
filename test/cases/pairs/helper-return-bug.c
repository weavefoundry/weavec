// RFC 0017: added regression pair. A separate helper returns the narrowed count one, not its incoming 257.
// UNITS: Inputs/numeric-helpers.c
// FLAGS: -std=c11
#include "Inputs/numeric-helpers.h"
void run(unsigned n) {
  if (n != 257) return;
  char *p = malloc(narrow_count(n));
  if (!p) return;
  p[1] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(257); return 0; }
