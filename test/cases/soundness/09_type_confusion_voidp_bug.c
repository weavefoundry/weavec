// Type confusion via void*: small object reinterpreted as larger struct.
// ASAN
#include <stdlib.h>
struct small { int x; };
struct big { int x; int y[16]; };
static int read_big(void *v) { struct big *b = v; return b->y[10]; }
int main(void) {
  struct small *s = malloc(sizeof *s);
  if (!s) return 1;
  s->x = 1;
  int r = read_big(s); // BUG: out-of-bounds
  free(s);
  return r;
}
