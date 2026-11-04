// A read through a pointer from one global that lands in the next: the
// bytes are the next global's, which the redzone guard passes; the next
// global has another colour (RFC 0037 §8), so the read traps.
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
