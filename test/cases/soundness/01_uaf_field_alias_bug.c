// UAF: two records alias the same buffer through fields; free via one, read via other.
// ASAN
#include <stdlib.h>
struct holder { char *buf; int n; };
int main(void) {
  struct holder *a = malloc(sizeof *a);
  struct holder *b = malloc(sizeof *b);
  if (!a || !b) return 1;
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  a->buf = p;
  b->buf = p;
  free(a->buf);
  int r = b->buf[0]; // BUG: use-after-free
  free(a); free(b);
  return r;
}
