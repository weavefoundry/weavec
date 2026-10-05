// RFC 0033 (V): a lowered definite violation on a path the program never takes does not trap.
// STAGE: S1
// FLAGS: -Wno-error=weavec-use-after-free
// RUN-INPUT: 0
#include <stdlib.h>
static int run(int n) {
  int *p = malloc(sizeof *p);
  if (p == NULL) return 1;
  *p = n;
  free(p);
  if (n > 0) return *p; // BUG: use-after-free
  return 0;
}
int main(int argc, char **argv) { return run(argc > 1 ? atoi(argv[1]) : 0); }
