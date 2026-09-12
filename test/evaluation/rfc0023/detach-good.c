#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static struct node *build(unsigned n) {
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
static struct node *pop(struct node **head) {
  struct node *p = *head;
  if (p) { *head = p->next; p->next = NULL; }
  return p;
}
static void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
int main(int argc, char **argv) { (void)argv; struct node *p=build((unsigned)argc), *q=pop(&p); destroy(p); if(q) { unsigned v=q->value; free(q); return v>100; } return 0; }
