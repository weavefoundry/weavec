// RFC 0030 §6.2: the analysis assumes the expression after the site, whatever its outcome.
// STAGE: S3
// The assertion 'n >= 0 && n < 8' is not proven, so it is checked. After the site the
// analysis assumes it (sound in the enforcing modes, which trap first), so buf[n] is proven
// in bounds and gets no index check: the unit has no checked spatial facet. The run passes
// n == 8, and the assertion traps before the access.
// RUN-INPUT:
// EXPECT-LEDGER: /summary/facets/spatial/checked == 0
#include <weavec.h>

int pick(int n) {
  char buf[8] = {0};
  WEAVEC_ASSUME(n >= 0 && n < 8); // TRAP: assert
  return buf[n];
}

int main(int argc, char **argv) {
  (void)argv;
  return pick(argc + 7);
}
