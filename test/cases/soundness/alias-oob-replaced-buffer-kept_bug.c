// RFC 0031 Motivation, probe p7 (build/rfc31/probes/p7.c): as alias-oob-replaced-buffer_bug.c,
// with both buffers kept in globals instead of freed. v0.11.0 proves the spatial facet of
// 'bx.buf[10]' (a false proof, ASan-confirmed); alias-oob-replaced-buffer-direct_bug.c is the
// control without the alias.
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
  q->buf = malloc(2 * sizeof(int));
  if (!q->buf) abort();
  bx.buf[10] = k;      /* heap overflow: bx.buf now has 2 ints */ // BUG: out-of-bounds // TRAP
  keep2 = bx.buf;
}
int main(int argc, char **argv) { (void)argv; s3(argc); puts("completed without trap"); return 0; }
