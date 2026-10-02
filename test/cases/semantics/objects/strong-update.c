// RFC 0031 §4.2, §4.7 I1: a store through a pointer that must point to one singular object is a strong update, seen through every alias.
// STAGE: S2
// 'q' must point to the local 'bx'. Freeing 'q->buf' and storing a fresh 2-int buffer
// through 'q' replaces the value of 'bx.buf' (the same cell), so 'bx.buf[1]' is an access
// to the live 2-int buffer: no use-after-free, in bounds, and one release of each buffer.
// CLEAN
// ASAN
#include <stdlib.h>
struct box { int *buf; };
int main(int argc, char **argv) {
  (void)argv;
  struct box bx;
  struct box *q = &bx;
  bx.buf = malloc(16 * sizeof(int));
  if (!bx.buf) return 1;
  free(q->buf);
  q->buf = malloc(2 * sizeof(int));
  if (!q->buf) return 1;
  bx.buf[1] = argc;
  int r = bx.buf[1];
  free(bx.buf);
  return r == argc ? 0 : 1;
}
