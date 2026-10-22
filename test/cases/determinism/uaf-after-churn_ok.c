// RFC 0036 §3: the twin of uaf-after-churn_bug.c, which forgets the block
// when it releases it; the churn reuses released blocks.
// CLEAN
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
  keep->fn();
  free(keep);
  keep = NULL;
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
  return keep == NULL ? 0 : 1;
}
