#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
static void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
int main(void) { destroy(NULL); return count(NULL)!=0; }
