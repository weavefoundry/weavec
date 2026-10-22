// A read through the address of a returned function's local: the frame's
// shadow is cleared at its return, so the read passes. RFC 0036 leaves use
// after return to a later RFC; this case pins the bug for it.
// XFAIL: RFC 0036 leaves spatial determinism and use after return to a later RFC
// RUN-INPUT:
#include <stdio.h>

__attribute__((noinline)) static int *leak(void) {
  int x[4] = {1, 2, 3, 4};
  int *volatile p = x;
  return p;
}

__attribute__((noinline)) static int clobber(void) {
  volatile int y[16];
  for (int i = 0; i < 16; i++)
    y[i] = 99;
  return y[3];
}

int main(void) {
  int *p = leak();
  clobber();
  printf("%d\n", p[1]); // TRAP
  return 0;
}
