#include <stdlib.h>
#include "node.h"
struct node *build(unsigned n) {
  struct node *head = NULL;
  for (unsigned i = 0; i < n; ++i) {
    struct node *p = malloc(sizeof *p);
    if (!p) break;
    p->value = i;
    if (i != 1) p->next = head;
    head = p;
  }
  return head;
}
unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
