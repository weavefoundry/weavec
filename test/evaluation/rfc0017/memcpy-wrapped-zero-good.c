// RFC 0017: added regression pair. A wrapped zero-byte copy preserves the old pointer; a complete copy installs null.
#include "../../Inputs/prelude.h"
void *memcpy(void *, const void *, size_t);
void run(void) {
  char *p = malloc(1);
  if (!p) return;
  char *source[1] = {NULL}, *destination[1] = {p};
  memcpy(destination, source, sizeof p);
  free(p);
  if (destination[0]) *destination[0] = 1;
}
