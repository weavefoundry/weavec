// Parameter aliases a global that the function frees.
// ASAN
#include <stdlib.h>
static char *g;
static int f(char *p) { free(g); g = NULL; return p[0]; } // BUG: use-after-free
int main(void) {
  g = calloc(8, 1);
  if (!g) return 1;
  return f(g);
}
