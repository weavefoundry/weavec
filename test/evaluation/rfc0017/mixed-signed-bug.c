// RFC 0017: added regression pair. The usual arithmetic conversions compare unsigned -1 with 1.
#include "../../Inputs/prelude.h"
void run(int delta, unsigned cap) {
  if (delta != -1 || cap != 1) return;
  char *p = malloc(1);
  if (!p) return;
  free(p);
  if (delta > cap) *p = 1; // BUG: mixed-signed
}
