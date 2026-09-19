// RFC 0030 §6.1: an assumption inside a region keeps its assertion facet and its check.
// STAGE: S3
// The region trusts raw memory operations, not assumptions: the Assume site's assertion is
// checked and emitted as __weavec_chk_assert. The run passes n == 0.
// RUN-INPUT:
#include <weavec.h>

int positive(int n) {
  WEAVEC_UNSAFE { WEAVEC_ASSUME(n > 0); } // TRAP: assert
  return n;
}

int main(int argc, char **argv) {
  (void)argv;
  return positive(argc - 1);
}
