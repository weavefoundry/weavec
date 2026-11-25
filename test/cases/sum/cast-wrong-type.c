// RFC 0054 §2.2: a `void *` context converted to a struct that is not
// there stops with `bad-cast` before its pointer field is read.
// FLAGS: -O2 -fweavec-safe
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

struct server { const char *WEAVEC_NULL_TERMINATED name; long ticks; };
struct counter { long a; long b; };

__attribute__((noinline)) char initial(void *context) {
  struct server *s = context; // TRAP: bad-cast
  return s->name[0];
}

int main(void) {
  struct counter *c = calloc(1, sizeof *c);
  if (c == NULL)
    return 1;
  c->a = 0x4141414141;
  printf("%c\n", initial(c));
  free(c);
  return 0;
}
