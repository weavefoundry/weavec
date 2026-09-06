// RFC 0014: independently identified pointer-identity evaluation.
#include <stdlib.h>
#include <string.h>
static void release_same(int *p, int *q) { if (p == q) free(p); }
void bad(int *p) { release_same(p, p);
  *p = 1; // BUG: pointer-guard
}
