#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
static struct node *build(unsigned n) {
  struct node *head = NULL;
  for (unsigned i = 0; i < n; ++i) {
    struct node *p = malloc(sizeof *p);
    if (!p) { destroy(head); return head; }
    p->value = i;
    p->next = head;
    head = p;
  }
  return head;
}
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
int main(int argc, char **argv) { (void)argv; struct node *p=build((unsigned)argc); unsigned n=count(p); destroy(p); return n>100; }
