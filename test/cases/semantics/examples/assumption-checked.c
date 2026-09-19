// RFC 0030 §4 "Assumptions" (a): an assumption the engine cannot prove becomes a checked assertion.
// STAGE: S3
// The assertion facet of the Assume site is checked, emitted as
// __weavec_chk_assert((n > 0) != 0). The run passes n == 0, so the assertion traps.
// RUN-INPUT:
#include <weavec.h>

int a(char *b, int n) { WEAVEC_ASSUME(n > 0); return b[n - 1]; } // TRAP: assert

int main(int argc, char **argv) {
  char buf[4] = {0};
  (void)argv;
  return a(buf, argc - 1);
}
