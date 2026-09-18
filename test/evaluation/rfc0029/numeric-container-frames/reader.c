#include "api.h"
unsigned read_tree(const struct node *n){return n?n->value+read_tree(n->child)+read_tree(n->next):0;}
void drop(struct node *n){while(n){struct node *next=n->next;drop(n->child);free(n->text);free(n);n=next;}}
