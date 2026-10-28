// RFC 0036 §3: a block released twice stops at the second release, even
// after enough churn that its slot could have been reused: it is not
// reused while the stale pointer remains.
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
  free(p); // TRAP: invalid-release
  printf("%d\n", p == q);
  return 0;
}
