// RFC 0054 §2.1 (c): the twin of container-wrong.c.
// CLEAN
// FLAGS: -O2 -fweavec-safe
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

struct link { struct link *next; };
struct item { const char *WEAVEC_NULL_TERMINATED name; struct link link; };

__attribute__((noinline)) char initial(struct link *l) {
  struct item *it = (struct item *)((char *)l - offsetof(struct item, link));
  return it->name[0];
}

int main(void) {
  struct item *it = calloc(1, sizeof *it);
  if (it == NULL)
    return 1;
  it->name = "weave";
  printf("%c\n", initial(&it->link));
  free(it);
  return 0;
}
