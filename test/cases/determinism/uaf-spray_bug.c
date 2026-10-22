// RFC 0036 §3: 64 MiB of allocations and releases of the same size never
// give the released block to a new owner while a pointer to it remains.
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
  for (long i = 0; i < 64L * 1024 * 1024 / 48; i++)
    free(malloc(48));
  char *q = malloc(48);
  if (q == NULL)
    return 1;
  strcpy(q, "attacker");
  printf("%d %c\n", p == q, p[0]); // TRAP: heap-use-after-free
  return 0;
}
