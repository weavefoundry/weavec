// RFC 0033 §1: a copy onto itself (libsodium's fe25519_copy(h, h)) leaves the bytes as they
// were in every supported C library; neither the analysis nor the overlap check stops it.
// STAGE: S7
// CLEAN
// RUN-INPUT: 0
#include <stdlib.h>
#include <string.h>
typedef long fe[5];
static void fe_copy(fe h, const fe f) { memcpy(h, f, 5 * sizeof h[0]); }
int main(int argc, char **argv) {
  fe a = {1, 2, 3, 4, 5}, b;
  if (argc < 2) return 2;
  fe_copy(b, a);
  fe_copy(a, a);
  long *p = argv[1][0] == '0' ? a : b;
  fe_copy(p, a);
  return (int)(a[4] - b[4]);
}
