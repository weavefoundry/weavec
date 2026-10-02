// RFC 0031 §8, Soundness A3: an owning cycle visible to the unit ('n->next = n') breaks the owner forest, so the destructor's proof does not hold at that call.
// STAGE: S4
// free_list is proven under A3 (list-destructor.c). Here the only node owns itself, so the
// second iteration reads 'p->next' from the freed node (ASan: heap-use-after-free in
// free_list) and frees it again. The call hands free_list a reachable owning cell that holds
// a pointer to its own object: an OwningCycle fact, unresolved(second-owner) at the call,
// so its temporal facet is not proven (the ASan site in free_list is covered by this
// enclosing frame, gate G4).
// ASAN
#include <stdlib.h>
struct node { struct node *next; int v; };
void free_list(struct node *p) {
  while (p) {
    struct node *next = p->next;
    free(p);
    p = next;
  }
}
int main(void) {
  struct node *n = malloc(sizeof *n);
  if (!n) abort();
  n->v = 1;
  n->next = n;
  free_list(n); // BUG: use-after-free // NOT-PROVEN: temporal
  return 0;
}
