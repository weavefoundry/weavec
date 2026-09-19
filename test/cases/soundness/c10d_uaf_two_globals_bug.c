// ASAN
#include <stdlib.h>
static char *g1, *g2;
static int drop_then_read(void) { free(g1); return g2[0]; } // BUG: use-after-free
int main(void) {
  g1 = calloc(8, 1);
  if (!g1) return 1;
  g2 = g1;
  return drop_then_read();
}
