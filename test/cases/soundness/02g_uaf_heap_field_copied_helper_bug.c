// Field freed by a helper; another helper reads it through a local copy.
// RFC 0031 amends RFC 0030 §9.4: a broken field class no longer breaks the
// record that holds it; the reads below rest on the field's entry value,
// which the engine follows through the copy and the arithmetic.
// ASAN
#include <stdlib.h>
struct s { char *buf; };
static void drop(struct s *o) { free(o->buf); }
static int peek(struct s *o) { char *b = o->buf; return b[0]; } // BUG: use-after-free // UNRESOLVED: temporal:dangling-escape // TRAP: live
static int peek_past(struct s *o) { char *b = o->buf + 1; return b[-1]; } // UNRESOLVED: temporal:dangling-escape
int main(void) {
  struct s o;
  o.buf = malloc(8);
  if (!o.buf) return 1;
  o.buf[0] = 1;
  drop(&o);
  if (o.buf == NULL)
    return peek_past(&o);
  return peek(&o);
}
