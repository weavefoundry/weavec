#include "api.h"
static char *make(void){return malloc(4);}
unsigned inspect(struct node *n){char *p=malloc(4);if(!p)return read_tree(n);n->text=p;free(p);return read_tree(n);}
