// A read through a pointer from one global that lands in the next: the
// bytes are the next global's, so the shadow passes it. Left to the later
// RFC on provenance bounds (RFC 0036).
// XFAIL: RFC 0036 leaves spatial determinism and use after return to a later RFC
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>

char name[32];
char secret[32] = "topsecret";

int main(int argc, char **argv) {
  char *p = name;
  long i = argc > 1 ? atol(argv[1]) : (long)(secret - name);
  printf("%c\n", p[i]); // TRAP: global-buffer-overflow
  return 0;
}
