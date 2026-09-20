// RFC 0030 §4 "Violation versus possible, temporal" (h): a release on some paths is a warning.
// STAGE: S3
// The record for 'p' reaches p[0] through a join with a predecessor that lacks it, so
// allPaths is false: the temporal facet is unresolved(may-released), with the warning "use
// of 'p' after it may have been freed". The null facet is checked and the program builds.
// The run with an argument takes the freeing path, which ASan reports.
// RUN-INPUT: x
// ASAN
#include <stdlib.h>

void h(char *p, int c) { if (c) free(p); p[0] = 1; } // BUG: use-after-free possible // UNRESOLVED: temporal:may-released

int main(int argc, char **argv) {
  char *p = malloc(8);
  (void)argv;
  if (!p) return 1;
  h(p, argc > 1);
  return 0;
}
