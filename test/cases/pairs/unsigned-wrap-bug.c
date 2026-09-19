// RFC 0017: added regression pair. UINT_MAX + 2 wraps to one before conversion to the allocation size.
// FLAGS: -std=c11
#include "Inputs/prelude.h"
void run(unsigned n) {
  if (n != (unsigned)-1) return;
  unsigned bytes = n + 2u;
  char *p = malloc(bytes);
  if (!p) return;
  p[1] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run((unsigned)-1); return 0; }
