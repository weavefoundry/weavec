// RFC 0031 Motivation, probe p6 (build/rfc31/probes/p6.c): 'bx.buf' holds 16 ints, is replaced
// through the alias 'q' by a 2-int buffer, and 'bx.buf[10]' is written: a heap overflow.
// v0.11.0 proves the spatial facet (the 16-int extent is kept for 'bx.buf'; a false proof,
// ASan-confirmed) and reports a false double-free on 'free(bx.buf)' (it believes 'bx.buf'
// is still 'old').
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
  bx.buf[10] = k;      /* heap overflow: bx.buf now has 2 ints */ // BUG: out-of-bounds // TRAP
  free(old);
  free(bx.buf);
}
int main(int argc, char **argv) { (void)argv; s3(argc); return 0; }
