// RFC 0054 §2.2: the twin of cast-wrong-type.c: the context is the struct
// it is converted to.
// CLEAN
// FLAGS: -O2 -fweavec-safe
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

struct server { const char *WEAVEC_NULL_TERMINATED name; long ticks; };

__attribute__((noinline)) char initial(void *context) {
  struct server *s = context;
  return s->name[0];
}

int main(void) {
  struct server *s = malloc(sizeof *s);
  if (s == NULL)
    return 1;
  s->name = "weave";
  s->ticks = 0;
  printf("%c\n", initial(s));
  free(s);
  return 0;
}
