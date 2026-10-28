// ASAN
#include <stdlib.h>
char *g;
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  g = p;
  free(p);
  return g[0]; // TRAP: heap-use-after-free
}
