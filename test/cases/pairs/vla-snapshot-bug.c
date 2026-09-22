// RFC 0017: added regression pair. sizeof the declared VLA stays four after its bound variable becomes eight.
// FLAGS: -std=c11
#include "Inputs/prelude.h"
void run(unsigned n) {
  if (n != 4) return;
  char array[n];
  n = 8;
  char *p = malloc(sizeof array);
  if (!p) return;
  p[n - 4] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(4); return 0; }
