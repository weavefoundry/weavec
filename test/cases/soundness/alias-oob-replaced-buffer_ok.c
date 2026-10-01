// RFC 0031 Motivation, correct twin of alias-oob-replaced-buffer_bug.c (probe p6): the write
// is within the 2-int buffer that replaced 'bx.buf' through the alias, and both buffers are
// freed once each.
// CLEAN
// ASAN
#include <stdlib.h>
struct box { int *buf; };
void s3(int k) {
  struct box bx;
  struct box *q = &bx;
  bx.buf = malloc(16 * sizeof(int));
  if (!bx.buf) abort();
  int *old = bx.buf;
  q->buf = malloc(2 * sizeof(int));
  if (!q->buf) abort();
  bx.buf[1] = k;
  free(old);
  free(bx.buf);
}
int main(int argc, char **argv) { (void)argv; s3(argc); return 0; }
