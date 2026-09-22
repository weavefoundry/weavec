// ASAN
#include <stdlib.h>
struct s { char *buf; };
static void drop(struct s *o) { free(o->buf); }
int main(void) {
  struct s o;
  o.buf = malloc(8);
  if (!o.buf) return 1;
  o.buf[0] = 1;
  drop(&o);
  return o.buf[0]; // BUG: use-after-free
}
