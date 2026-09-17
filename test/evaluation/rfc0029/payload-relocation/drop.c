#include "api.h"
#include <stdlib.h>
void drop(struct Node *n){while(n){struct Node *next=n->next;drop(n->child);free(n->text);free(n->name);free(n);n=next;}}
