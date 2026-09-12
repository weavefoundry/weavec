#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
int main(void) { struct node a; a.value=1; return count(&a)!=1; }
