// RFC 0034 section 6.3 (exclusive stores of one fresh object), a variant of
// exclusive-out-params.c with three exits: each case of the switch stores
// its own fresh string through one of three out-parameters (the summary
// keeps them as possible stores), so no two of a, b and c hold one object.
// CLEAN
// ALLOW: double-free
// RUN-INPUT:
// RUN-INPUT: x
// RUN-INPUT: x y
#include <stdlib.h>
#include <string.h>

static void three(int w, char **a, char **b, char **c) {
  switch (w) {
  case 0: *a = strdup("a"); break;
  case 1: *b = strdup("b"); break;
  default: *c = strdup("c"); break;
  }
}

int main(int argc, char **argv) {
  char *a = NULL, *b = NULL, *c = NULL;
  (void)argv;
  three(argc - 1, &a, &b, &c);
  free(a);
  free(b);
  free(c);
  return 0;
}
