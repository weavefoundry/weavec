// An index from `a` that lands in the live block `b` next to it: the bytes
// it touches are `b`'s, so the shadow passes it. RFC 0036 leaves bounds from
// a pointer's provenance to a later RFC; this case pins the bug for it.
// XFAIL: RFC 0036 leaves spatial determinism and use after return to a later RFC
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  int *a = malloc(64), *b = malloc(64);
  if (a == NULL || b == NULL)
    return 1;
  b[0] = 0;
  // The attacker's index: the distance to b, unless one is given.
  long i = argc > 1 ? atol(argv[1]) : (long)(b - a);
  a[i] = 0x41414141; // TRAP: heap-buffer-overflow
  printf("%x\n", b[0]);
  return 0;
}
