#include <stdlib.h>
int main(void) {
  int *a = malloc(4 * sizeof *a);
  if (!a) return 1;
  int r = a[2]; // BUG: use-of-uninitialized // NEUTRALISED: zero-init
  free(a);
  return r;
}
