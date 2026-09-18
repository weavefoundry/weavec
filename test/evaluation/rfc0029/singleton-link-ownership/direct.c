#include "api.h"
void build(struct node *p,struct reader *r){struct node *q=make();if(!q)return;p->child=q;r->position=1;}
