// RFC 0031 §4.5 D3/D6, §4.6: the list destructor 'while (p) { next = p->next; free(p); p = next; }' is proven.
// STAGE: S4
// Each iteration materialises the current node, releases it and moves to a node loaded
// from its owning slot 'next', which D6 keeps distinct; the released node is collected at
// the loop head. No temporal facet of the program is left unresolved (v0.11.0 leaves the
// read of 'p->next' and 'free(p)' as may-alias-released).
// Not met: the read of 'p->next' and 'free(p)' stay may-alias-released, never proven
// (test/cases/KNOWN-DIFFERENCES.md, *Cases*; RFC 0031 *Unresolved questions*).
// CLEAN
// ASAN
// EXPECT-LEDGER: /summary/facets/temporal/unresolved == 2
// EXPECT-LEDGER: /summary/unresolvedReasons/may-alias-released == 2
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
  struct node *h = NULL;
  for (int i = 0; i < 3; i++) {
    struct node *n = malloc(sizeof *n);
    if (!n) abort();
    n->v = i;
    n->next = h;
    h = n;
  }
  free_list(h);
  return 0;
}
