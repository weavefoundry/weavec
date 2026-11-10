// RFC 0041 detection: an off-by-one copy in safe code stops at the store;
// the fixed twin runs clean.
// DETECT: -DFIX
// FLAGS: -O2
#include <weavec.h>
#include <stddef.h>
#include <stdio.h>

WEAVEC_SAFE size_t copy(char *WEAVEC_COUNTED_BY(cap) dst, size_t cap,
                        const char *WEAVEC_NULL_TERMINATED src) {
  size_t i = 0;
#ifdef FIX
  for (; src[i] && i + 1 < cap; i++)
#else
  for (; src[i] && i < cap; i++)
#endif
    dst[i] = src[i];
  dst[i] = 0; // STOP
  return i;
}

int main(void) {
  char name[8];
  size_t n = copy(name, sizeof name, "overflowing");
  printf("%s %zu\n", name, n);
  return 0;
}
