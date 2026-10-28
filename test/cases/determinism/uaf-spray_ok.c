// RFC 0036 §3: the twin of uaf-spray_bug.c: with no pointer left to the
// released block, the spray reuses it.
// CLEAN
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *p = malloc(48);
  if (p == NULL)
    return 1;
  strcpy(p, "secret");
  free(p);
  p = NULL;
  for (long i = 0; i < 64L * 1024 * 1024 / 48; i++)
    free(malloc(48));
  char *q = malloc(48);
  if (q == NULL)
    return 1;
  strcpy(q, "attacker");
  printf("%c\n", q[0]);
  free(q);
  return 0;
}
