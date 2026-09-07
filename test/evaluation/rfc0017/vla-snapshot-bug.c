// RFC 0017: added regression pair. sizeof the declared VLA stays four after its bound variable becomes eight.
#include "../../Inputs/prelude.h"
void run(unsigned n) {
  if (n != 4) return;
  char array[n];
  n = 8;
  char *p = malloc(sizeof array);
  if (!p) return;
  p[n - 4] = 0; // BUG: vla-snapshot
  free(p);
}
