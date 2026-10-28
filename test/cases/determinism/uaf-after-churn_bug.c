// RFC 0036 §3: a released block stays unaddressable until a sweep finds no
// pointer to it, however much the program allocates meanwhile: the call
// through the stale pointer stops.
// RUN-INPUT:
#include <stdio.h>
#include <stdlib.h>

struct handler {
  void (*fn)(void);
  char pad[40];
};

static void greet(void) { puts("hello"); }

static struct handler *keep;

int main(void) {
  keep = malloc(sizeof *keep);
  if (keep == NULL)
    return 1;
  keep->fn = greet;
  free(keep);
  enum { Count = 600000 };
  void **live = malloc(sizeof(void *) * Count);
  if (live == NULL)
    return 1;
  for (int i = 0; i < Count; i++)
    live[i] = malloc(48);
  for (int i = 0; i < Count; i += 2)
    free(live[i]);
  for (int i = 0; i < Count; i += 2)
    live[i] = malloc(48);
  keep->fn(); // TRAP: heap-use-after-free
  return 0;
}
