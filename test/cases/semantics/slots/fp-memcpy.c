// RFC 0030 §8 and §9.3: 'fp = memcpy' applies memcpy's row to the indirect call.
// STAGE: S7
// The static function pointer 'copy' is a closed slot whose only target is memcpy, so the
// row applies exactly: the destination requirement bytes(n) is checked against the exact
// extent of 'dst' with the len template. The run copies 8 bytes into 4.
// RUN-INPUT: 8
// ASAN
#include <stdlib.h>
#include <string.h>

static void *(*copy)(void *, const void *, size_t) = memcpy;

int main(int argc, char **argv) {
  char dst[4];
  size_t n = argc > 1 ? (size_t)atoi(argv[1]) : sizeof dst;
  copy(dst, "abcdefgh", n); // BUG: out-of-bounds // TRAP: len
  return dst[0] == 'a' ? 0 : 1;
}
