// RFC 0030 §7.5: an access after a call that may not return is not a must-access.
// STAGE: S6
// 'check_or_die' exits when its argument is 0, so its summary says it may not return. p[0]
// after the call is therefore not a must-access and 'first' gets no requirement: the call
// first(NULL, 0) runs no nonnull check, and the program exits with status 2 before the
// access. No error, no trap.
// CLEAN
// ASAN
#include <stdio.h>
#include <stdlib.h>

static void check_or_die(int ok) {
  if (!ok) {
    fputs("no data\n", stderr);
    exit(2);
  }
}

static int first(const int *p, int ok) {
  check_or_die(ok);
  return p[0];
}

int main(int argc, char **argv) {
  static const int one = 1;
  (void)argv;
  return first(argc > 1 ? &one : NULL, argc > 1) == 1 ? 0 : 1;
}
