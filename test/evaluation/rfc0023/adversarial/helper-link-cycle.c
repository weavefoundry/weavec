#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *n=p->next; free(p); p=n; } }
static void corrupt(struct node *p) { p->next=p; } int main(void) { struct node a={1,0}; corrupt(&a); return count(&a); }
