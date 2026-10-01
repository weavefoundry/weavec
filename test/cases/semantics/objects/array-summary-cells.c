// RFC 0031 §4.2: array summary cells: elements read before the release loop are live.
// STAGE: S2
// As array-summary-cells_bug.c, with 'a[0]' read before the elements are released: no
// use-after-free, and each element is released once.
// CLEAN
// ASAN
#include <stdlib.h>
struct n { int v; };
int main(void) {
  struct n *a[4];
  for (int i = 0; i < 4; i++) {
    a[i] = malloc(sizeof *a[i]);
    if (!a[i]) abort();
    a[i]->v = i;
  }
  struct n *first = a[0];
  int r = first->v;
  for (int i = 0; i < 4; i++)
    free(a[i]);
  return r;
}
