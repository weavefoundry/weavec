// RFC 0032 §4: the correct twin of stack-cursor_bug.c: seven bytes and a terminator fit 'buf'.
// STAGE: S4
// CLEAN
// RUN-INPUT: 0123456
// ASAN
#include <stdio.h>
static void fill(char *dst, const char *src) {
  while (*src)
    *dst++ = *src++; // GUARDED: spatial
  *dst = 0;
}
int main(int argc, char **argv) {
  char buf[8];
  if (argc < 2) return 1;
  fill(buf, argv[1]);
  return buf[0] == 0;
}
