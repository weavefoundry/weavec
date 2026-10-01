// RFC 0031 Motivation, correct twin of alias-oob-replaced-buffer-direct_bug.c (probe p7ctl):
// the write is within the 2-int buffer.
// CLEAN
// ASAN
#include <stdlib.h>
#include <stdio.h>
struct box { int *buf; };
int *keep1, *keep2;
__attribute__((noinline)) void s3(int k) {
  struct box bx;
  struct box *q = &bx;
  bx.buf = malloc(16 * sizeof(int));
  if (!bx.buf) abort();
  keep1 = bx.buf;
  bx.buf = malloc(2 * sizeof(int));
  if (!q->buf) abort();
  bx.buf[1] = k;
  keep2 = bx.buf;
}
int main(int argc, char **argv) { (void)argv; s3(argc); puts("completed"); return 0; }
