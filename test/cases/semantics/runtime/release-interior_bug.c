// RFC 0032 §3: a release of a pointer that is not the start of its block traps.
// STAGE: S5
// The offset is not known to the analysis (may-invalid-release, unknown-index); the guard
// asks the allocator.
// RUN-INPUT: 4
// ASAN
#include <stdlib.h>
static void drop(char *p, int skip) {
  free(p + skip); // BUG: invalid-release // TRAP
}
int main(int argc, char **argv) {
  char *p = malloc(32);
  if (!p || argc < 2) return 1;
  drop(p, atoi(argv[1]));
  return 0;
}
