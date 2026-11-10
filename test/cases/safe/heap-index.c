// RFC 0041 §4: an allocation's bounds are its requested size, so an index
// past it stops even inside the block's slot.
// FLAGS: -O2
#include <weavec.h>
#include <stdlib.h>

WEAVEC_SAFE int last(size_t n) {
  int *a = calloc(n, sizeof *a);
  int r = a[n]; // TRAP: pointer-out-of-bounds
  free(a);
  return r;
}

int main(void) { return last(5); }
