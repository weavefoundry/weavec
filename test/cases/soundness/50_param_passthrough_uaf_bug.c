// Exported function: frees one param and reads another which may alias (no callers here).
// ASAN
#include <stdlib.h>
int release_then_read(char *a, char *b) { free(a); return b[0]; } // BUG: use-after-free
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  return release_then_read(p, p);
}
