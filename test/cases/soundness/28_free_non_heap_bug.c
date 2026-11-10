// ASAN
#include <stdlib.h>
static void release(void *p) { free(p); } // TRAP: invalid-free
int main(void) {
  int x = 0;
  int *p = &x;
  release(p);
  return 0;
}
