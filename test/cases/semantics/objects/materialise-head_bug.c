// RFC 0031 §4.6 (materialisation): releasing the materialised head releases the object the list's head cell still points to.
// STAGE: S4
// As materialise-pop.c, but the head is freed without unlinking it: 'l.head' still points
// to the released node (ASan: heap-use-after-free).
// ASAN
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
  struct node *second = n->next;
  free(n);
  int r = l.head->v; // BUG: use-after-free // NOT-PROVEN: temporal
  while (second) {
    struct node *next = second->next;
    free(second);
    second = next;
  }
  return r;
}
