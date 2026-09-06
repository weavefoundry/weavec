// RFC 0014: independently identified pointer-identity evaluation.
#include <stdlib.h>
#include <string.h>
void clean(void) { int *p = malloc(sizeof *p), *q; if (!p) return; memcpy(&q, &p, sizeof p); *q = 1; free(q); }
