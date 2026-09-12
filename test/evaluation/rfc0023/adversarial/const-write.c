#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *n=p->next; free(p); p=n; } }
static void change(struct node *p) { while(p) { p->value=1; p=p->next; } } int main(void) { static const struct node a={1,0}; change((struct node*)&a); return 0; }
