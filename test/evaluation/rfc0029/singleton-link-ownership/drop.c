#include "api.h"
void drop(struct node *p){while(p){struct node *n=p->next;if(!(p->flags&1))drop(p->child);free(p);p=n;}}
