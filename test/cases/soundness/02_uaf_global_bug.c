// UAF: pointer saved in a global by one helper, freed by caller, read by another helper.
// ASAN
#include <stdlib.h>
static char *g_cache;
static void remember(char *p) { g_cache = p; }
static int peek(void) { return g_cache[0]; } // BUG: use-after-free // NOT-PROVEN: temporal
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 3;
  remember(p);
  free(p);
  return peek();
}
