#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *n=p->next; free(p); p=n; } }
int main(void) { struct node a={1,0}, b={2,&a}; a.next=&b; return count(&a); }
