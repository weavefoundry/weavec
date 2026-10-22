// RFC 0036 §3: the twin of double-free-after-churn_bug.c, which releases
// each block once.
// CLEAN
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  char *p = malloc(48);
  if (p == NULL)
    return 1;
  free(p);
  for (long i = 0; i < 64L * 1024 * 1024 / 48; i++)
    free(malloc(48));
  char *q = malloc(48);
  free(q);
  puts("ok");
  return 0;
}
