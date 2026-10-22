// The twin of use-after-longjmp_bug.c, which keeps the value in a global
// rather than a pointer into the abandoned frame.
// CLEAN
// RUN-INPUT:
#include <setjmp.h>
#include <stdio.h>

static jmp_buf there;
static int saved;

__attribute__((noinline)) static void deep(int n) {
  int local[8] = {0};
  local[0] = n;
  int *volatile p = local;
  saved = p[0];
  longjmp(there, 1);
}

int main(void) {
  for (int round = 0; round < 1000; round++)
    if (setjmp(there) == 0)
      deep(round);
  printf("%d\n", saved);
  return 0;
}
