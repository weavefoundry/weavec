#include <stdlib.h>
struct slot { int *p; unsigned n; };
static void release(struct slot *s) { free(s->p); }
int main(void) {
  struct slot a[1]; a->p = malloc(sizeof(int));
  release(a); return 0;
}
