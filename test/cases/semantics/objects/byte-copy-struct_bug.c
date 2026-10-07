// RFC 0031 §4.2 (byte-wise writes): a whole-cell memcpy between objects of compatible layout copies the pointer symbols, so a release through the copy is seen through the original.
// STAGE: S2
// 'copy' is a byte-wise copy of 'orig'; 'copy.buf' is freed and 'orig.buf' is read
// (ASan: heap-use-after-free). v0.10.0-era engines leave such copies raw-cast (probe 42);
// the object engine copies the symbol, so the value is exact.
// ASAN
#include <stdlib.h>
#include <string.h>
struct box { int *buf; int n; };
int main(void) {
  struct box orig, copy;
  orig.buf = malloc(4 * sizeof(int));
  if (!orig.buf) return 1;
  orig.n = 4;
  orig.buf[0] = 1;
  memcpy(&copy, &orig, sizeof orig);
  free(copy.buf);
  return orig.buf[0]; // BUG: use-after-free
}
