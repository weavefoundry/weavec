#include "api.h"
void drop(struct node *p){while(p){struct node *n=p->next;if(!(p->flags&256))free(p->payload);free(p);p=n;}}
