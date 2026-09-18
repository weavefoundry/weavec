#include <stdlib.h>
struct slot { int *p; unsigned n; };
int main(void) {
  struct slot a[2];
  a->p = malloc(sizeof(int));
  a[1].p = malloc(sizeof(int));
  if (a[0].p) *(*a).p = 7;
  if (a[1].p) *a[1].p = 9;
  free((*a).p); free(a[1].p);
  return 0;
}
