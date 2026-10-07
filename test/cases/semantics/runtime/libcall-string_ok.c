// RFC 0032 §6: the correct twin of libcall-string_bug.c: seven characters and their terminator fit the block.
// STAGE: S5
// CLEAN
// RUN-INPUT: 0123456
// ASAN
#include <stdlib.h>
#include <string.h>
static void put(char *dst, const char *src) {
  strcpy(dst, src);
}
int main(int argc, char **argv) {
  char *dst = malloc(8);
  if (!dst || argc < 2) return 1;
  put(dst, argv[1]);
  int r = dst[0] == 0;
  free(dst);
  return r;
}
