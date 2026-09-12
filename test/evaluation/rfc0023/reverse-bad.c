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
static struct node *reverse(struct node *p) {
  struct node *out = NULL;
  while (p) {
    struct node *next = p->next;
    p->next = p;
    out = p;
    p = next;
  }
  return out;
}
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
static void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
int main(int argc, char **argv) { (void)argv; struct node *p=reverse(build((unsigned)argc)); unsigned n=count(p); destroy(p); return n>100; }
