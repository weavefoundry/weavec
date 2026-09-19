// RFC 0030 §4 "Violation, spatial": an access past an exact allocation extent is an error.
// STAGE: S3
// p[4] is out of bounds for every value the facts allow ('p[4]' is out of bounds: ... an
// object of 4 bytes), so the unit produces no object. ASan confirms the overflow.
// ASAN
#include <stdlib.h>

void f(void) { char *p = malloc(4); if (!p) return; p[4] = 0; free(p); } // BUG: out-of-bounds definite

int main(void) {
  f();
  return 0;
}
