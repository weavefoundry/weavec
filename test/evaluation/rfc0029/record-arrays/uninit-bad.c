#include <stdlib.h>
struct slot { int *p; unsigned n; };
int main(void) {
  struct slot a[2]; a[1].p = 0; free(a->p); return 0;
}
