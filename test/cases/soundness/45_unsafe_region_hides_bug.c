// ASAN
// XFAIL: an unsafe region turns the guards off: the use after free runs unchecked (ASan reports it)
#include <stdlib.h>
#include <weavec.h>
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  WEAVEC_UNSAFE { free(p); p[1] = 2; } // TRAP // UNGUARDED: unsafe
  return 0;
}
