// RFC 0031 Motivation, correct twin of alias-null-box-field_bug.c (probe p5 s2): the store
// through the alias puts the buffer back, so 'bx.buf' is non-null when it is read.
// CLEAN
// ASAN
#include <stdlib.h>
struct box { int *buf; };
int s2(void) {
  struct box bx;
  bx.buf = NULL;
  struct box *q = &bx;
  int *o = malloc(4 * sizeof(int));
  if (!o) abort();
  o[0] = 7;
  q->buf = o;
  int r = bx.buf[0];
  free(o);
  return r;
}
int main(void) { return s2() == 7 ? 0 : 1; }
