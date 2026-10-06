// RFC 0034 section 6.3 (exclusive stores of one fresh object), a variant of
// exclusive-out-params.c: pick() stores a fresh string through *a on one
// path and returns another on the other, so the result and *a never hold
// the same object. Freeing both is correct for every argument count.
// CLEAN
// ALLOW: double-free
// RUN-INPUT:
// RUN-INPUT: x
#include <stdlib.h>
#include <string.h>

static char *pick(int w, char **a) {
  if (w) { *a = strdup("a"); return NULL; }
  return strdup("b");
}

int main(int argc, char **argv) {
  char *a = NULL;
  (void)argv;
  char *r = pick(argc > 1, &a);
  free(a);
  free(r);
  return 0;
}
