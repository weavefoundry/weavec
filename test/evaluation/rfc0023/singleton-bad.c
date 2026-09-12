#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static void destroy(struct node *p) { while(p) { free(p); p=p->next; } }
int main(void) { struct node *p=malloc(sizeof *p); if(!p) return 0; p->value=1; p->next=NULL; destroy(p); return 0; }
