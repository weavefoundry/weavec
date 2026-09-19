// Alias kept across realloc that moves the block.
// ASAN
#include <stdlib.h>
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  char *alias = p;
  char *q = realloc(p, 1 << 20);
  if (!q) { free(p); return 1; }
  int r = alias[0]; // BUG: use-after-move
  free(q);
  return r;
}
