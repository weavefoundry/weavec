#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *n=p->next; free(p); p=n; } }
int main(void) { char raw[3*sizeof(struct node)]={0}; struct node *p=(struct node*)(raw+1); return count(p); }
