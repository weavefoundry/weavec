// ASAN
#include <stdlib.h>
static char *g1, *g2;
static void cleanup(void) { free(g1); free(g2); } // TRAP: invalid-release
int main(void) {
  g1 = malloc(8);
  g2 = g1;
  cleanup();
  return 0;
}
