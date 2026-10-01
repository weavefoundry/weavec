// RFC 0031 §4.2 (byte-wise writes): after a whole-cell memcpy of a struct holding a pointer, the copy and the original hold the same value.
// STAGE: S2
// 'orig.buf' is read before the buffer is freed through 'copy.buf', and it is freed once:
// no use-after-free, no double free and no leak.
// CLEAN
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
  int r = orig.buf[0] + copy.buf[0];
  free(copy.buf);
  return r == 2 ? 0 : 1;
}
