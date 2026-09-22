// Only half of a pointer copied: result is a forged pointer.
// ASAN
#include <stdlib.h>
#include <string.h>
int main(void) {
  char *p = calloc(8, 1);
  if (!p) return 1;
  char *q = NULL;
  memcpy(&q, &p, 4);
  int r = q ? q[0] : 0; // BUG: out-of-bounds // NOT-PROVEN: spatial
  free(p);
  return r;
}
