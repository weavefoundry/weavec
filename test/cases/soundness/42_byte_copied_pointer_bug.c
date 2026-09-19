// Pointer copied byte-by-byte, original freed, copy used.
// RFC 0030 §2.3: `q` is made by a byte-wise copy, so `q[0]` is
// `unresolved(raw-cast)`. The marker names the reason because the `return`
// on the same line is an exit site whose own temporal facet is proven.
// ASAN
#include <stdlib.h>
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  char *q;
  unsigned char *src = (unsigned char *)&p, *dst = (unsigned char *)&q;
  for (size_t i = 0; i < sizeof p; i++) dst[i] = src[i];
  free(p);
  return q[0]; // BUG: use-after-free // UNRESOLVED: temporal:raw-cast
}
