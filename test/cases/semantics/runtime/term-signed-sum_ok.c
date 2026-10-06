// RFC 0030 §10.2, term arithmetic: a length the program computes in `int`
// is checked at the value the program computes. `count + 8` with
// `count == -5` is 3; a negative leaf inside a sum is not a huge length
// (FLAC__MD5Final's padding, which trapped).
// CLEAN
// RUN-INPUT:
#include <stdlib.h>
#include <string.h>
static void pad(unsigned char *in, unsigned used) {
  int count = (int)(used & 0x3f);
  unsigned char *p = in + count;
  *p++ = 0x80;
  count = 56 - 1 - count;
  if (count < 0) {
    memset(p, 0, count + 8);
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
  pad(in, used);
  int last = in[63];
  free(in);
  return last;
}
