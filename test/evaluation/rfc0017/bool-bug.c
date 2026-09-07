// RFC 0017: added regression pair. _Bool preserves nonzeroness, not the low bit of 256.
#include "../../Inputs/prelude.h"
void run(unsigned n) {
  if (n != 256) return;
  _Bool stored = n;
  char *p = malloc(1);
  if (!p) return;
  free(p);
  if (stored == 1) *p = 1; // BUG: bool
}
