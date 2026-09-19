// Struct copy carries the pointer; original freed; copy used.
// ASAN
#include <stdlib.h>
struct holder { char *buf; int n; };
int main(void) {
  struct holder h1;
  h1.buf = malloc(8);
  if (!h1.buf) return 1;
  h1.buf[0] = 1;
  h1.n = 8;
  struct holder h2 = h1;
  free(h1.buf);
  return h2.buf[0]; // BUG: use-after-free
}
