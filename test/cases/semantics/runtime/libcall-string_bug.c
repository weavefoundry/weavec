// RFC 0032 §6: a string requirement is guarded by looking for the terminator inside the argument's object, and a need of 'strlen' by reading it there.
// STAGE: S5
// 'put' copies a string it knows nothing about into a block it knows nothing about.
// RUN-INPUT: 0123456789abcdef
// ASAN
#include <stdlib.h>
#include <string.h>
static void put(char *dst, const char *src) {
  strcpy(dst, src); // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
}
int main(int argc, char **argv) {
  char *dst = malloc(8);
  if (!dst || argc < 2) return 1;
  put(dst, argv[1]);
  int r = dst[0] == 0;
  free(dst);
  return r;
}
