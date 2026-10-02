// RFC 0031 §4.2 (recency abstraction): an older allocation of a site, released in the loop, stays released in the site's summary.
// STAGE: S2
// As recency-loop.c, but 'old' keeps a pointer to the node freed last; after the loop it
// points to a released node (ASan: heap-use-after-free), so its use may not be proven.
// RUN-INPUT: 3
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int main(int argc, char **argv) {
  int count = argc > 1 ? atoi(argv[1]) : 0;
  struct n *prev = NULL, *old = NULL;
  int sum = 0;
  for (int i = 0; i < count; i++) {
    struct n *cur = malloc(sizeof *cur);
    if (!cur) abort();
    cur->v = i;
    cur->next = NULL;
    if (prev) {
      old = prev;
      free(prev);
    }
    prev = cur;
  }
  if (old)
    sum += old->v; // BUG: use-after-free // NOT-PROVEN: temporal
  free(prev);
  return sum;
}
