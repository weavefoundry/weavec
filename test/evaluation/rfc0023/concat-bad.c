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
static struct node *concat(struct node *a, struct node *b) {
  if (!a) return b;
  struct node *p = a;
  while (p->next) p = p->next;
  p->next = b;
  return a;
}
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
static void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
int main(int argc, char **argv) { (void)argv; struct node *a=build((unsigned)argc); struct node *p=concat(a,a); unsigned n=count(p); destroy(p); return n>100; }
