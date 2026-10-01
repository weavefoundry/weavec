// RFC 0031 Motivation, correct twin of alias-oob-box-field_bug.c (probe p5 s1): the index is
// within the 4-int buffer.
// CLEAN
// ASAN
#include <stdlib.h>
struct box { int *buf; };
int s1(struct box *bp) {
  int *o = malloc(4 * sizeof(int));
  if (!o) abort();
  o[3] = 7;
  bp->buf = o;
  struct box *q = bp;
  return q->buf[3];
}
int main(void) {
  struct box b;
  int r = s1(&b);
  free(b.buf);
  return r == 7 ? 0 : 1;
}
