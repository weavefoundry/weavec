#include "api.h"
void drop(struct node *p){while(p){struct node *next=p->next;if(!(p->flags&256))drop(p->child);free(p);p=next;}}
