// RFC 0030 §3.4, as amended by RFC 0033 (V): a definite out-of-bounds store whose error is lowered to a warning still traps.
// STAGE: S5
// With -Wno-error=weavec-out-of-bounds the definite violation p[4] is reported as a warning
// and the unit produces an object. In the enforcing modes the planner guards the site
// anyway: a violation decided at the access from an exact extent keeps the unconditional
// violation trap, which fires whenever the access is reached (RFC 0033 (V) guards only
// violations decided from a model of other code).
// FLAGS: -Wno-error=weavec-out-of-bounds
// RUN-INPUT:
// ASAN
#include <stdlib.h>

int main(void) {
  char *p = malloc(4);
  if (!p) return 1;
  p[4] = 0; // BUG: out-of-bounds possible // TRAP
  free(p);
  return 0;
}
