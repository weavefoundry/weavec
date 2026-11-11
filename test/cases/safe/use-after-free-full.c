// RFC 0041 §7: -fweavec-safe-checks=full keeps a safe function's guards, so
// a use after free in it is reported as in guarded code.
// FLAGS: -O1 -fweavec-safe-checks=full
#include <weavec.h>
#include <stdlib.h>

WEAVEC_SAFE int stale(void) {
  int *a = calloc(4, sizeof *a);
  a[0] = 42;
  free(a);
  return a[0]; // TRAP: heap-use-after-free
}

int main(void) { return stale(); }
