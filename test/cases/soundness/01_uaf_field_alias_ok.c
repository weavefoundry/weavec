// CLEAN
// ASAN
#include <stdlib.h>
struct holder { char *buf; int n; };
int main(void) {
  struct holder *a = malloc(sizeof *a);
  struct holder *b = malloc(sizeof *b);
  if (!a || !b) { free(a); free(b); return 1; }
  char *p = malloc(8);
  if (!p) { free(a); free(b); return 1; }
  p[0] = 1;
  a->buf = p;
  b->buf = p;
  int r = b->buf[0];
  free(a->buf);
  free(a); free(b);
  return r;
}
