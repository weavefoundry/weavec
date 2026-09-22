// RFC 0030 §6.1: a possible temporal finding inside a region is reported exactly as outside it.
// STAGE: S3
// The release on some paths gives the warning with "may" wording and the temporal facet
// unresolved(may-released), as in examples/temporal-possible.c.
#include <stdlib.h>
#include <weavec.h>

void h(char *p, int c) {
  WEAVEC_UNSAFE {
    if (c) free(p);
    p[0] = 1; // BUG: use-after-free possible // UNRESOLVED: temporal:may-released
  }
}
