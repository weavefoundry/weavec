// Declaration says BORROWED; the real (unseen) definition frees. Trusted annotation.
// UNITS: 38_extern_impl.c
// ASAN
#include <stdlib.h>
void inspect(char * p);
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  inspect(p);
  int r = p[0]; // TRAP
  free(p);
  return r;
}
