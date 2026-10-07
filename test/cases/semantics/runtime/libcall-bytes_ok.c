// RFC 0032 §6: the correct twin of libcall-bytes_bug.c: 16 bytes fit both blocks, and a zero-length copy needs no object at all.
// STAGE: S5
// CLEAN
// RUN-INPUT: 16
// RUN-INPUT: 0
// ASAN
#include <stdlib.h>
#include <string.h>
static void copy(char *d, const char *s, size_t n) {
  memcpy(d, s, n);
}
int main(int argc, char **argv) {
  char *src = calloc(1, 64);
  char *dst = malloc(16);
  if (!src || !dst || argc < 2) return 1;
  size_t n = (size_t)atoi(argv[1]);
  /* One past the end is a valid destination for no bytes. */
  copy(n ? dst : dst + 16, src, n);
  int r = dst[0];
  free(src);
  free(dst);
  return r;
}
