// RFC 0030 §10.2: a negative count field is an extent of 0, so every access through it traps.
// STAGE: S6
// Term arithmetic never lets C's conversions turn a negative count into a huge unsigned one:
// a signed leaf used as an extent or count is 0 when negative. With len == -1 the index
// check of x->d[i] fails for every i, although d[0] lies inside the buffer.
// RUN-INPUT:
#include "../Inputs/rfc0030.h"

struct b { char *WEAVEC_COUNTED_BY(len) d; int len; };

char at(const struct b *x, int i) { return x->d[i]; } // TRAP: index

int main(int argc, char **argv) {
  char buf[4] = "abc";
  struct b x = {buf, -1};
  (void)argv;
  return at(&x, argc - 1);
}
