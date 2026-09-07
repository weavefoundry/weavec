// RFC 0014: independently identified pointer-identity evaluation.
#include <stdlib.h>
#include <string.h>
static void release_same(int *p, int *q) { if (p == q) free(p); }
void clean(int *p, int *q) { if (p != q) { release_same(p, q); *p = 1; free(p); } }
