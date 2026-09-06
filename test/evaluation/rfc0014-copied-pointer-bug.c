// RFC 0014: independently identified pointer-identity evaluation.
#include <stdlib.h>
#include <string.h>
void bad(int *p) { int *q; memcpy(&q, &p, sizeof p); free(p);
  *q = 1; // BUG: copied-pointer
}
