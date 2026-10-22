// A read through a local of a frame `longjmp` abandoned: the stack's shadow
// is cleared before the jump (RFC 0035 §3.5), so the read passes. A use
// after return, left to a later RFC (RFC 0036).
// XFAIL: RFC 0036 leaves spatial determinism and use after return to a later RFC
// RUN-INPUT:
#include <setjmp.h>
#include <stdio.h>

static jmp_buf there;
static int *saved;

__attribute__((noinline)) static void deep(int n) {
  int local[8] = {0};
  local[0] = n;
  saved = local;
  longjmp(there, 1);
}

int main(void) {
  if (setjmp(there) == 0) {
    deep(7);
    return 1;
  }
  printf("%d\n", saved[0]); // TRAP
  return 0;
}
