// RFC 0032 §6: a library call's byte requirement, unresolved for want of an extent, is guarded with its need.
// STAGE: S5
// 'copy' has no idea how large either block is. The guards check 'n' bytes from each
// argument inside its own object.
// RUN-INPUT: 24
// ASAN
#include <stdlib.h>
#include <string.h>
static void copy(char *d, const char *s, size_t n) {
  memcpy(d, s, n); // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
}
int main(int argc, char **argv) {
  char *src = calloc(1, 64);
  char *dst = malloc(16);
  if (!src || !dst || argc < 2) return 1;
  copy(dst, src, (size_t)atoi(argv[1]));
  int r = dst[0];
  free(src);
  free(dst);
  return r;
}
