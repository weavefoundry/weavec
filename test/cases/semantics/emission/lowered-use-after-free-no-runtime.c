// RFC 0030 §3.4, as amended by RFC 0033 (V): without the runtime a lowered temporal violation
// traps unconditionally.
// STAGE: S5
// Temporal facets have no static check, and without the runtime no guard, so a definite
// use-after-free whose error is lowered to a warning gets
// __builtin_verbose_trap("weavec", "violation") before the operation.
// FLAGS: -Wno-error=weavec-use-after-free -fno-weavec-runtime
// RUN-INPUT:
// ASAN
#include <stdlib.h>

int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  free(p);
  p[0] = 1; // BUG: use-after-free possible // TRAP: violation
  return 0;
}
