#include <stdlib.h>
struct slot { int *p; unsigned n; };
int main(void) {
  struct slot a[1]; a->p = 0; a[1].p = 0; return 0;
}
