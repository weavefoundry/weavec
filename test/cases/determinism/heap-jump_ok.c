// The twin of heap-jump_bug.c, its index inside `a`.
// CLEAN
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  int *a = malloc(64), *b = malloc(64);
  if (a == NULL || b == NULL)
    return 1;
  b[0] = 0;
  long i = argc > 1 ? atol(argv[1]) : 15;
  a[i] = 0x41414141;
  printf("%x %x\n", a[15], b[0]);
  return 0;
}
