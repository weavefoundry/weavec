// Global freed by one helper, read by another (no parameters involved).
// ASAN
#include <stdlib.h>
static char *g;
static void init(void) { g = malloc(8); if (g) g[0] = 1; }
static void shutdown(void) { free(g); }
static int use(void) { return g ? g[0] : 0; } // BUG: use-after-free // NOT-PROVEN: temporal
int main(void) {
  init();
  shutdown();
  return use();
}
