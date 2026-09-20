// RFC 0030 §3.4: a definite out-of-bounds store whose error is lowered to a warning still traps.
// STAGE: S5
// With -Wno-error=weavec-out-of-bounds the definite violation p[4] is reported as a warning
// and the unit produces an object. In the enforcing modes the planner guards the site
// anyway: the spatial violation gets its facet's check, an index check against the
// allocation's 4 bytes, which traps whenever the violation happens, as it always does here.
// FLAGS: -Wno-error=weavec-out-of-bounds
// RUN-INPUT:
// ASAN
#include <stdlib.h>

int main(void) {
  char *p = malloc(4);
  if (!p) return 1;
  p[4] = 0; // BUG: out-of-bounds possible // TRAP: index
  free(p);
  return 0;
}
