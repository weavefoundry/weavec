// Library callback (qsort comparator) frees memory the caller later reads.
// ASAN
#include <stdlib.h>
static char *g_victim;
static int cmp(const void *a, const void *b) {
  free(g_victim); g_victim = NULL;
  return *(const int *)a - *(const int *)b;
}
int main(void) {
  int xs[2] = {2, 1};
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  g_victim = p;
  qsort(xs, 2, sizeof xs[0], cmp);
  return p[0]; // BUG: use-after-free
}
