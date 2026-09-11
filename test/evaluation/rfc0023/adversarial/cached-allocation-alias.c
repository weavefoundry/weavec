/* RFC 0023: supplemental adversarial regression. */
#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *next=p->next; free(p); p=next; } }
static struct node *cached;
static struct node *get(void) { if(!cached) { cached=malloc(sizeof *cached); if(cached) { cached->value=1; cached->next=0; } } return cached; }
int main(void) { struct node *a=get(), *b=get(); destroy(a); return count(b); }
