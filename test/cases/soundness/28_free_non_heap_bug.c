// ASAN
#include <stdlib.h>
static void release(void *p) { free(p); }
int main(void) {
  int x = 0;
  int *p = &x;
  release(p); // BUG: invalid-release
  return 0;
}
