// RFC 0042 T-A1: a typed function trusts what a guarded one stored; with
// -fweavec-safe-checks=full it keeps its guards, and the read past the block
// that the guarded function's count let it make is reported.
// FLAGS: -O2 -fweavec-typed -fweavec-safe-checks=full
#include <weavec.h>
#include <stdlib.h>

struct vec { int *WEAVEC_COUNTED_BY(len) data; size_t len; };

int at(const struct vec *v, size_t i) {
  return v->data[i]; // TRAP: heap-buffer-overflow
}

WEAVEC_GUARDED void lie(struct vec *v) { v->len = 16; }

WEAVEC_GUARDED int main(void) {
  struct vec v = {calloc(8, sizeof(int)), 8};
  lie(&v);
  return at(&v, 10);
}
