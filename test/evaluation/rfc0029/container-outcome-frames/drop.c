#include "api.h"
void drop(struct node *n){while(n){struct node *next=n->next;drop(n->child);if(!(n->flags&1))free(n->text);free(n);n=next;}}
