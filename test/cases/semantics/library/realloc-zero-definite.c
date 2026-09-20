// RFC 0030 §8.2 (realloc): with a size known to be zero, realloc releases its argument on the null class.
// STAGE: S4
// realloc(free) releases 'p' on the non-null result class, and on the null class when the
// size is zero, because glibc's realloc(p, 0) frees 'p' and returns null (the case is
// 'outcome null a0 freed when param 1 =0'). With the size known to be zero,
// 'q = realloc(p, 0); if (!q) free(p);' is a definite double free.
#include <stdlib.h>

void shrink(char *p) {
  char *q = realloc(p, 0);
  if (!q) free(p); // BUG: double-free definite
  free(q);
}
