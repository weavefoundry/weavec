// CLEAN
// ASAN
#include <stdlib.h>
struct node { int v; struct node *next; };
static void list_free(struct node *h) {
  struct node *n = h;
  while (n) { struct node *nx = n->next; free(n); n = nx; }
}
int main(void) {
  struct node *a = malloc(sizeof *a), *b = malloc(sizeof *b);
  if (!a || !b) { free(a); free(b); return 1; }
  a->v = 1; a->next = b; b->v = 2; b->next = NULL;
  list_free(a);
  return 0;
}
