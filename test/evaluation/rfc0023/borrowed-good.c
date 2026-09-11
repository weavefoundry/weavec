#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
int main(void) { struct node b={2,NULL}, a={1,&b}; return count(&a)!=2; }
