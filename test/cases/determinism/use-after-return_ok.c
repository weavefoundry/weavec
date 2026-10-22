// The twin of use-after-return_bug.c, which copies the values out before
// it returns.
// CLEAN
// RUN-INPUT:
#include <stdio.h>

__attribute__((noinline)) static int second(void) {
  int x[4] = {1, 2, 3, 4};
  int *volatile p = x;
  return p[1];
}

__attribute__((noinline)) static int clobber(void) {
  volatile int y[16];
  for (int i = 0; i < 16; i++)
    y[i] = 99;
  return y[3];
}

int main(void) {
  int v = second();
  clobber();
  printf("%d\n", v);
  return 0;
}
