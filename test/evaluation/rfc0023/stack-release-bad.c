#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
int main(void) { struct node a={1,NULL}; destroy(&a); return 0; }
