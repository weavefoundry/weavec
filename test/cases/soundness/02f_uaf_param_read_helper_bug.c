// ASAN
#include <stdlib.h>
static int peek(char *p) { return p[0]; }
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  free(p);
  return peek(p); // BUG: use-after-free
}
