// CLEAN
// ASAN
#include <stdlib.h>
static char *g_cache;
static void remember(char *p) { g_cache = p; }
static int peek(void) { return g_cache ? g_cache[0] : 0; }
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 3;
  remember(p);
  int r = peek();
  g_cache = NULL;
  free(p);
  return r;
}
