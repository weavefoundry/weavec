// RFC 0030 §4 "Checked, null": unknown nullness costs a nonnull check and nothing else.
// STAGE: S6
// Both Deref sites of 'second' have null checked. Spatial is proven because 'n' is Single
// under A1 and the slot 'node.next' is Single (§7.3); temporal is proven. Emitted:
// ((struct node *)__weavec_chk_nonnull(((struct node *)__weavec_chk_nonnull(n))->next))->v.
// The driver passes a node whose 'next' is null, so the outer dereference traps.
// RUN-INPUT:
// EXPECT-LEDGER: /summary/facets/spatial/unresolved == 0
// EXPECT-LEDGER: /summary/facets/temporal/unresolved == 0
#include <stddef.h>

struct node { struct node *next; int v; };

int second(struct node *n) { return n->next->v; } // TRAP: nonnull

int main(int argc, char **argv) {
  struct node b = {NULL, 2};
  struct node a = {argc > 1 ? &b : NULL, 1};
  (void)argv;
  return second(&a);
}
