// RFC 0030 §3.4, as amended by RFC 0033 (V): a lowered temporal violation is guarded.
// STAGE: S5
// With the runtime, a definite use-after-free whose error is lowered to a warning gets the
// runtime's `live` guard, which traps when the object is dead. Without the runtime it traps
// unconditionally (lowered-use-after-free-no-runtime.c).
// FLAGS: -Wno-error=weavec-use-after-free
// RUN-INPUT:
// ASAN
#include <stdlib.h>

int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  free(p);
  p[0] = 1; // BUG: use-after-free possible // TRAP: live
  return 0;
}
