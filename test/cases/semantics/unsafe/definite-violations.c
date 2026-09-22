// RFC 0030 §6.1: WEAVEC_UNSAFE no longer hides definite violations.
// STAGE: S3
// Temporal state is tracked inside a region exactly as outside, so a definite
// use-after-free in an unsafe function is still an error (probe 45), and a definite spatial
// violation against an exact extent inside an unsafe block is still an error. No diagnostic
// is dropped for being inside a region.
#include <stdlib.h>
#include <weavec.h>

WEAVEC_UNSAFE void drop(char *p) {
  free(p);
  p[0] = 0; // BUG: use-after-free definite
}

int past(void) {
  char b[4] = {0};
  WEAVEC_UNSAFE { b[4] = 1; } // BUG: out-of-bounds definite
  return b[0];
}
