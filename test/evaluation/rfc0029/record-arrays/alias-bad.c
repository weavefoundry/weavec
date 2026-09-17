#include <stdlib.h>
struct slot { int *p; unsigned n; };
int main(void) {
  struct slot a[2]; a->p = malloc(sizeof(int));
  a[1].p = (*a).p; free(a[1].p); free(a->p); return 0;
}
