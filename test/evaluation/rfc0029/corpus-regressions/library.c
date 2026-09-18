#include "api.h"
#include <stdlib.h>
void destroy(struct node *p) {while(p){struct node *next=p->next;destroy(p->child);free(p);p=next;}}
