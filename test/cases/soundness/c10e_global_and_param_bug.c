// ASAN
#include <stdlib.h>
static char *g;
static void drop_both(char *p) { free(p); free(g); } // BUG: double-free
int main(void) {
  g = malloc(8);
  drop_both(g);
  return 0;
}
