#include <stdlib.h>
#include "node.h"
struct node *build(unsigned n) {
  struct node *head = NULL;
  for (unsigned i = 0; i < n; ++i) {
    struct node *p = malloc(sizeof *p);
    if (!p) break;
    p->value = i;
    p->next = head;
    head = p;
  }
  return head;
}
struct node *pop(struct node **head) {
  struct node *p = *head;
  if (p) { *head = p->next; p->next = NULL; }
  return p;
}
void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
