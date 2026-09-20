// CLEAN
// ASAN
#include <stdlib.h>
struct small { int x; };
struct big { int x; int y[16]; };
static int read_small(void *v) { struct small *b = v; return b->x; }
int main(void) {
  struct small *s = malloc(sizeof *s);
  if (!s) return 1;
  s->x = 1;
  int r = read_small(s);
  free(s);
  return r;
}
