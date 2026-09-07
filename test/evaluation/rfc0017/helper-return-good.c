// RFC 0017: added regression pair. A separate helper returns the narrowed count one, not its incoming 257.
#include "numeric-helpers.h"
void run(unsigned n) {
  if (n != 257) return;
  char *p = malloc(narrow_count(n));
  if (!p) return;
  p[0] = 0;
  free(p);
}
