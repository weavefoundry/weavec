// RFC 0032 §3: the correct twin of release-interior_bug.c: offset 0 is the start of the block.
// STAGE: S5
// CLEAN
// ALLOW: invalid-release
// RUN-INPUT: 0
// ASAN
#include <stdlib.h>
static void drop(char *p, int skip) {
  free(p + skip); // GUARDED: spatial
}
int main(int argc, char **argv) {
  char *p = malloc(32);
  if (!p || argc < 2) return 1;
  drop(p, atoi(argv[1]));
  return 0;
}
