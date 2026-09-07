// RFC 0017: added regression pair. UINT_MAX + 2 wraps to one before conversion to the allocation size.
#include "../../Inputs/prelude.h"
void run(unsigned n) {
  if (n != (unsigned)-1) return;
  unsigned bytes = n + 2u;
  char *p = malloc(bytes);
  if (!p) return;
  p[0] = 0;
  free(p);
}
