// ASAN
#include <stdlib.h>
int main(void) {
  char *p = calloc(16, 1);
  if (!p) return 1;
  char *q = realloc(p, 2);
  if (!q) { free(p); return 1; }
  int r = q[5]; // BUG: out-of-bounds
  free(q);
  return r;
}
