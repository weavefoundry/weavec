// RFC 0017: added regression pair. An eight-bit storage conversion makes 256 zero.
#include "../../Inputs/prelude.h"
void run(unsigned n) {
  if (n != 256) return;
  unsigned char stored = n;
  char *p = malloc(1);
  if (!p) return;
  free(p);
  if (stored != 0) *p = 1;
}
