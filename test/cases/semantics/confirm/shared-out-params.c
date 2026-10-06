// RFC 0034 section 6.3 (exclusive stores of one fresh object), the
// counterpart of exclusive-out-params.c: share() stores one fresh object
// through both out-parameters on every path, so a and b hold the same
// object and freeing both is a definite double free. So is a call whose
// argument decides parse()'s exclusive stores.
#include <stdlib.h>
#include <string.h>

static void share(char **a, char **b) {
  char *p = malloc(4);
  *a = p;
  *b = p;
}

static void parse(int which, char **a, char **b) {
  if (which) *a = strdup("a");
  else       *b = strdup("b");
}

int shared(void) {
  char *a = NULL, *b = NULL;
  share(&a, &b);
  free(a);
  free(b); // BUG: double-free definite
  return 0;
}

int decided(void) {
  char *a = NULL, *b = NULL;
  parse(1, &a, &b);
  char *c = a;
  free(a);
  return *c; // BUG: use-after-free definite
}
