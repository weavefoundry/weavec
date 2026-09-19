// RFC 0030 §4 "Violation versus possible, temporal" (g): a release on every path is an error.
// STAGE: S3
// The record for 'p' is definite (allPaths, not conditional, not from an unknown callee),
// so p[0] is a temporal violation: "use of 'p' after it was freed".
// ASAN
#include <stdlib.h>

void g(char *p) { free(p); p[0] = 1; } // BUG: use-after-free definite

int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  g(p);
  return 0;
}
