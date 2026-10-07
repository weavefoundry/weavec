// RFC 0031 §4.2, §4.7 I1, I3: a release through one alias is a fact on the object, seen through the other.
// STAGE: S2
// 'q' must point to the local 'bx'; 'free(q->buf)' releases the one object 'bx.buf' holds,
// and 'bx.buf' is read afterwards without being replaced. The value is exact, so the
// finding is expected to be definite; the ledger row alone would also report it.
// ASAN
#include <stdlib.h>
struct box { int *buf; };
int main(void) {
  struct box bx;
  struct box *q = &bx;
  bx.buf = malloc(4 * sizeof(int));
  if (!bx.buf) return 1;
  bx.buf[0] = 1;
  free(q->buf);
  return bx.buf[0]; // BUG: use-after-free
}
