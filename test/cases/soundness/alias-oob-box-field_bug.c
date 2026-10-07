// RFC 0031 Motivation, probe p5 s1 (build/rfc31/probes/p5.c): a 4-int buffer is stored in
// 'bp->buf' and indexed at 10 through the alias 'q'. v0.11.0 reports it (definite
// out-of-bounds); a check (trap) would also report it.
// ASAN
#include <stdlib.h>
struct box { int *buf; };
int s1(struct box *bp) {
  int *o = malloc(4 * sizeof(int));
  if (!o) abort();
  bp->buf = o;
  struct box *q = bp;
  return q->buf[10]; // BUG: out-of-bounds // TRAP
}
int main(void) {
  struct box b;
  int r = s1(&b);
  free(b.buf);
  return r == 12345;
}
