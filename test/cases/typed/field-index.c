// RFC 0042 §4: an element of a counted field is compared with the count
// loaded beside the pointer, wherever the struct came from.
// FLAGS: -O2 -fweavec-safe
#include <weavec.h>
#include <stdlib.h>

struct vec { int *WEAVEC_COUNTED_BY(len) data; size_t len; };

int at(const struct vec *v, size_t i) {
  return v->data[i]; // TRAP: pointer-out-of-bounds
}

WEAVEC_GUARDED int main(void) {
  struct vec v = {calloc(10, sizeof(int)), 4};
  return at(&v, 4); // the block has 10 ints; the count says 4
}
