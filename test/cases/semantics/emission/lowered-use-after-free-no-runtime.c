// RFC 0034 §6.4: a lowered violation gets the check or guard its facet would have as a possible
// finding, never an unconditional trap. A temporal facet has no static check, and without the
// runtime no guard, so a definite use-after-free whose error is lowered to a warning inserts
// nothing and the facet is unresolved(lowered).
// STAGE: S5
// FLAGS: -Wno-error=weavec-use-after-free -fno-weavec-runtime
#include <stdlib.h>

int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  free(p);
  p[0] = 1; // BUG: use-after-free possible // UNRESOLVED: temporal:lowered
  return 0;
}
