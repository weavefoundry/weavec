// RFC 0042 §6: a pointer stored in a counted field must cover the count it
// is paired with (`bounds-conversion`).
// FLAGS: -O2 -fweavec-typed
#include <weavec.h>
#include <stdlib.h>

struct vec { int *WEAVEC_COUNTED_BY(len) data; size_t len; };

void set(struct vec *v, size_t n) {
  int *d = calloc(n, sizeof *d);
  v->data = d;
  v->len = n + 1; // TRAP: bounds-conversion
}

WEAVEC_GUARDED int main(void) {
  struct vec v = {0, 0};
  set(&v, 3);
  return v.data[0];
}
