// RFC 0054 §2.1 (c): container_of to a struct the link is not in stops with
// `bad-cast`.
// FLAGS: -O2 -fweavec-safe
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

struct link { struct link *next; };
struct item { const char *WEAVEC_NULL_TERMINATED name; struct link link; };
struct other { long a; struct link link; };

__attribute__((noinline)) char initial(struct link *l) {
  struct item *it = (struct item *)((char *)l - offsetof(struct item, link)); // TRAP: bad-cast
  return it->name[0];
}

int main(void) {
  struct other *o = calloc(1, sizeof *o);
  if (o == NULL)
    return 1;
  o->a = 0x4141414141;
  printf("%c\n", initial(&o->link));
  free(o);
  return 0;
}
