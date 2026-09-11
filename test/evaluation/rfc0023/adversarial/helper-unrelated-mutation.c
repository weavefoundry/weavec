/* RFC 0023: supplemental adversarial regression. */
#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *next=p->next; free(p); p=next; } }
static void change(struct node *p, struct node **out, struct node *q) { while(p) { p->value=1; p=p->next; } *out=q; }
int main(void) { struct node a={1,0}, b={2,0}; count(&b); change(&a,&b.next,&b); return count(&b); }
