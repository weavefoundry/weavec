// Classic: free node then read its next.
// ASAN
#include <stdlib.h>
struct node { int v; struct node *next; };
static void list_free(struct node *h) {
  for (struct node *n = h; n; n = n->next) free(n); // BUG: use-after-free
}
int main(void) {
  struct node *a = malloc(sizeof *a), *b = malloc(sizeof *b);
  if (!a || !b) return 1;
  a->v = 1; a->next = b; b->v = 2; b->next = NULL;
  list_free(a);
  return 0;
}
