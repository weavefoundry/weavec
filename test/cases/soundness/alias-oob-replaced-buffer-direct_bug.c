// RFC 0031 Motivation, probe p7ctl (build/rfc31/probes/p7ctl.c): the control of
// alias-oob-replaced-buffer-kept_bug.c, with 'bx.buf' replaced directly instead of through
// the alias. v0.11.0 reports it (definite out-of-bounds: index 10 of 8 bytes).
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
  bx.buf[10] = k;      /* heap overflow: bx.buf now has 2 ints */ // BUG: out-of-bounds // TRAP
  keep2 = bx.buf;
}
int main(int argc, char **argv) { (void)argv; s3(argc); puts("completed without trap"); return 0; }
