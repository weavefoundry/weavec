/* RFC 0023: supplemental adversarial regression. */
#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *next=p->next; free(p); p=next; } }
struct item { char *data; struct item *link; };
static void clear(struct item *p) { while(p) { struct item *next=p->link; free(p->data); free(p); p=next; } }
int main(void) { struct item *p=malloc(sizeof *p); if(!p) return 0; p->data=malloc(4); p->link=0; char *saved=p->data; clear(p); free(saved); return 0; }
