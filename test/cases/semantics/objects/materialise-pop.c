// RFC 0031 §4.6 (materialisation): releasing the head of a list built at one allocation site focuses it out of the site's summary; the next node stays live.
// STAGE: S4
// All nodes come from one allocation site in a loop. Popping the head ('n = l.head;
// l.head = n->next; free(n)') releases the materialised head only, so the new head, loaded
// from its owning slot 'next' (D6), is live and its use is proven.
// The destructor loop after it is not (test/cases/KNOWN-DIFFERENCES.md, *Cases*):
// 'l.head->next' and 'free(l.head)' stay may-alias-released, never proven.
// CLEAN
// ASAN
// EXPECT-LEDGER: /summary/facets/temporal/unresolved == 2
// EXPECT-LEDGER: /summary/unresolvedReasons/may-alias-released == 2
#include <stdlib.h>
struct node { struct node *next; int v; };
struct list { struct node *head; };
int main(void) {
  struct list l = { NULL };
  for (int i = 0; i < 3; i++) {
    struct node *n = malloc(sizeof *n);
    if (!n) abort();
    n->v = i;
    n->next = l.head;
    l.head = n;
  }
  struct node *n = l.head;
  l.head = n->next;
  free(n);
  int r = l.head->v;
  while (l.head) {
    struct node *next = l.head->next;
    free(l.head);
    l.head = next;
  }
  return r == 1 ? 0 : 1;
}
