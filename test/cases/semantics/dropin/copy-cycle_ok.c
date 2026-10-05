// RFC 0033 §1: locals copied into each other in a loop before a release do not send the
// analysis into an endless walk (libevent's regress_buffer.c crashed weavec-cc).
// STAGE: S7
// CLEAN
// RUN-INPUT:
#include <stdlib.h>
int main(void) {
  char *a = malloc(4), *b = malloc(4), *t;
  if (a == NULL || b == NULL) return 1;
  for (int i = 0; i < 3; i++) { t = a; a = b; b = t; }
  free(a);
  free(b);
  return 0;
}
