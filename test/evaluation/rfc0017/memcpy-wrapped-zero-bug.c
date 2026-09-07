// RFC 0017: added regression pair. A wrapped zero-byte copy preserves the old pointer; a complete copy installs null.
#include "../../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void run(void) {
  char *p = malloc(1);
  if (!p) return;
  char *source[1] = {NULL}, *destination[1] = {p};
  size_t count = (size_t)-1 / sizeof p + 1;
  memcpy(destination, source, count * sizeof p);
  free(p);
  if (destination[0]) *destination[0] = 1; // BUG: memcpy-wrapped-zero
}
