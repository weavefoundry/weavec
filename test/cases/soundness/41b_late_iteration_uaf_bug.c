// UAF on a late loop iteration decided by the induction variable.
// ASAN
#include <stdlib.h>
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 0;
  int s = 0;
  for (int i = 0; i < 100; i++) {
    if (i == 50) free(p);
    if (i == 60) s += p[0]; // BUG: use-after-free
  }
  return s;
}
