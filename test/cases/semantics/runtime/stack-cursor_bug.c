// RFC 0032 §4: a local whose address escapes is a tracked object, so a cursor that walks off its end traps at the first byte past it.
// STAGE: S4
// 'fill' knows nothing about the extent of 'dst'. 'buf' is registered when it is declared;
// the address one past its end still identifies it, so the store there fails its guard.
// RUN-INPUT: 0123456789abcdef
// ASAN
#include <stdio.h>
static void fill(char *dst, const char *src) {
  while (*src)
    *dst++ = *src++; // BUG: out-of-bounds // TRAP
  *dst = 0;
}
int main(int argc, char **argv) {
  char buf[8];
  if (argc < 2) return 1;
  fill(buf, argv[1]);
  return buf[0] == 0;
}
