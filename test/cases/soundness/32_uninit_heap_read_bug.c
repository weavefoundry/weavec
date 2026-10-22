// Zero-initialisation (RFC 0035 §1) defines the read: the run is clean.
// CLEAN
#include <stdlib.h>
int main(void) {
  int *a = malloc(4 * sizeof *a);
  if (!a) return 1;
  int r = a[2];
  free(a);
  return r;
}
