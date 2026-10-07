// RFC 0031 §4.2 (recency abstraction): the most recent allocation of a site is a singular object distinct from the older ones folded into the site's summary.
// STAGE: S2
// Each iteration allocates 'cur', frees the previous node and keeps 'cur'. After the loop
// 'prev' is the most recent allocation, which no release reached: its use is proven, and
// the released older nodes do not taint it.
// RUN-INPUT: 3
// CLEAN
// ASAN
#include <stdlib.h>
struct n { struct n *next; int v; };
int main(int argc, char **argv) {
  int count = argc > 1 ? atoi(argv[1]) : 0;
  struct n *prev = NULL;
  int sum = 0;
  for (int i = 0; i < count; i++) {
    struct n *cur = malloc(sizeof *cur);
    if (!cur) abort();
    cur->v = i;
    cur->next = NULL;
    if (prev) {
      sum += prev->v;
      free(prev);
    }
    prev = cur;
  }
  if (prev) {
    sum += prev->v;
    free(prev);
  }
  return sum == 3 ? 0 : 1;
}
