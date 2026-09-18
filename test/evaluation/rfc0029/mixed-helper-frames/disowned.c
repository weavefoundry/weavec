#include "api.h"
void build(struct node *p,struct reader *r){struct node *q=make();if(!q)return;p->child=q;p->flags=1;r->position=1;}
