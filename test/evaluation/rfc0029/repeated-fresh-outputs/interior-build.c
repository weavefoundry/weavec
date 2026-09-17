#include "api.h"
int build(struct node *p,struct reader *r){struct node *head=0,*tail=0;while(r->offset<r->length&&r->data[r->offset]){struct node *q=make();if(!q){drop(head);return 0;}q++;if(!head){head=tail=q;}else{tail->next=q;q->prev=tail;tail=q;}mark(tail);r->offset++;}if(head)head->prev=tail;p->child=head;return 1;}
