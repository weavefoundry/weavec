// RFC 0017: added regression pair. Both upper bounds of the helper loop determine its caller requirement.
#include "numeric-helpers.h"
void run(void) {
  char *p = malloc(4);
  if (!p) return;
  fill_min(p, 5, 9); // BUG: min-bound
  free(p);
}
