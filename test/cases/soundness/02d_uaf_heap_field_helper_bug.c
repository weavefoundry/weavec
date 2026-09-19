// Field freed by a helper, another helper reads through the same object.
// ASAN
#include <stdlib.h>
struct s { char *buf; };
static void drop(struct s *o) { free(o->buf); }
static int peek(struct s *o) { return o->buf[0]; } // BUG: use-after-free // NOT-PROVEN: temporal
int main(void) {
  struct s o;
  o.buf = malloc(8);
  if (!o.buf) return 1;
  o.buf[0] = 1;
  drop(&o);
  return peek(&o);
}
