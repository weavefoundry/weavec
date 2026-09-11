/* RFC 0023: supplemental adversarial regression. */
#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static unsigned count(const struct node *p) { unsigned n=0; while(p) { ++n; p=p->next; } return n; }
static void destroy(struct node *p) { while(p) { struct node *next=p->next; free(p); p=next; } }
int main(int argc, char **argv) { (void)argv; struct node *head=0; for(int i=0;i<argc;++i) { struct node *p=malloc(sizeof *p); if(!p) break; p->value=1; if(i) p->next=head; head=p; } return count(head); }
