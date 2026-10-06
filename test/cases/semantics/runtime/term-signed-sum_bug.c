// RFC 0030 §10.2, term arithmetic: a length the program computes in `int`
// is checked at the value the program computes. The twin of
// term-signed-sum_ok.c pads one byte too many: `count + 9` is 4 bytes from
// the 61st, one past the end of the 64-byte block, found at the call.
// RUN-INPUT:
// ASAN
#include <stdlib.h>
#include <string.h>
static void pad(unsigned char *in, unsigned used) {
  int count = (int)(used & 0x3f);
  unsigned char *p = in + count;
  *p++ = 0x80;
  count = 56 - 1 - count;
  if (count < 0) {
    memset(p, 0, count + 9);
    p = in;
    count = 56;
  }
  memset(p, 0, count);
}
int main(void) {
  volatile unsigned used = 60;
  unsigned char *in = malloc(64);
  if (!in)
    return 1;
  pad(in, used); // BUG: out-of-bounds
  int last = in[63];
  free(in);
  return last;
}
