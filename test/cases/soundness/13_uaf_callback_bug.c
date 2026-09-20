// UAF through a callback invoked via a function pointer stored in a struct.
// ASAN
#include <stdlib.h>
struct ctx { char *buf; void (*on_done)(struct ctx *); };
static void release_buf(struct ctx *c) { free(c->buf); }
static void finish(struct ctx *c) { c->on_done(c); }
int main(void) {
  struct ctx c;
  c.buf = malloc(8);
  if (!c.buf) return 1;
  c.buf[0] = 1;
  c.on_done = release_buf;
  finish(&c);
  return c.buf[0]; // BUG: use-after-free
}
