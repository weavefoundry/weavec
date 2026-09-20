// Declaration says BORROWED; the real (unseen) definition frees. Trusted annotation.
// UNITS: 38_extern_impl.c
// ASAN
#include <stdlib.h>
#include <weavec.h>
void inspect(char *WEAVEC_BORROWED p); // BUG: annotation-mismatch definite
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  inspect(p);
  int r = p[0];
  free(p);
  return r;
}
