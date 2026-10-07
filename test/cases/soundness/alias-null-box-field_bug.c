// RFC 0031 Motivation, probe p5 s2 (build/rfc31/probes/p5.c): 'bx.buf' is set to a fresh
// buffer, then to NULL through the alias 'q', then dereferenced as 'bx.buf[0]'.
// v0.11.0 proves the null facet of 'bx.buf[0]' (a false proof, ASan-confirmed as a SEGV);
// the value is exactly NULL, so a definite error or a nonnull trap reports it.
// ASAN
#include <stdlib.h>
struct box { int *buf; };
int s2(void) {
  struct box bx;
  bx.buf = NULL;
  struct box *q = &bx;
  int *o = malloc(4 * sizeof(int));
  if (!o) abort();
  bx.buf = o;
  q->buf = NULL;
  return bx.buf[0];   /* null deref */ // BUG: null-dereference // TRAP
}
int main(void) { return s2() == 12345; }
