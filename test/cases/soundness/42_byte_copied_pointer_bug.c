// Pointer copied byte-by-byte, original freed, copy used.
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
  return q[0]; // BUG: use-after-free // NOT-PROVEN: temporal
}
