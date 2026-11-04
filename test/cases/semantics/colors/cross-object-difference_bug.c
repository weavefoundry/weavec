// RFC 0037 accepted false traps: `p + (q - p)` with p and q in different
// objects is undefined in C; the access is computed from p and lands in
// q's object, so it traps (ASan lets it pass).
// RUN-INPUT:
#include <stdlib.h>

static volatile long zero;

int main(void) {
  char *p = malloc(32);
  char *q = malloc(32);
  const long delta = q - p + zero;
  q[0] = 1;
  return p[delta]; // TRAP: heap-buffer-overflow
}
