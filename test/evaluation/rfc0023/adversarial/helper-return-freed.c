/* RFC 0023: supplemental adversarial regression. */
#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *next=p->next; free(p); p=next; } }
static struct node *bad(struct node *p) { struct node *saved=p; destroy(p); return saved; }
int main(void) { struct node *p=malloc(sizeof *p); if(!p) return 0; p->value=1; p->next=0; return count(bad(p)); }
