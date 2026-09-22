// realloc(p, 0) (may free p, UB in C23), then old pointer freed again / used.
// ASAN
#include <stdlib.h>
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  char *q = realloc(p, 0);
  (void)q;
  free(p); // BUG: use-after-move
  return 0;
}
