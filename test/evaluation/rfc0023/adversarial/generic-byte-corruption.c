#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *n=p->next; free(p); p=n; } }
#include <string.h>
unsigned f(struct node *p) { if(p) memset(p,127,sizeof *p); return count(p); }
