// ASAN
#include <stdlib.h>
#include <weavec.h>
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  WEAVEC_UNSAFE { free(p); p[1] = 2; } // BUG: use-after-free definite
  return 0;
}
