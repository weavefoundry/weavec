// Only half of a pointer copied: result is a forged pointer.
// ASAN
// The forged pointer has no provenance (RFC 0036 A6) and points outside every
// object, into the first 4 GiB, which neither Darwin (its zero page) nor a
// position-independent executable on Linux maps: it stops as a fault.
#include <stdlib.h>
#include <string.h>
int main(void) {
  // Not the first block of its class, whose address may be a multiple of
  // 4 GiB (its low half, and so the forged pointer, would then be null).
  char *first = calloc(8, 1);
  char *p = calloc(8, 1);
  if (!p || !first) return 1;
  char *q = NULL;
  memcpy(&q, &p, 4);
  int r = q ? q[0] : 0; // TRAP
  free(p);
  return r;
}
