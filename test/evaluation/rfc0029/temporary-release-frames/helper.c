#include "api.h"
static char *make(void){return malloc(4);}
unsigned inspect(struct node *n){char *p=make();if(!p)return read_tree(n);free(p);return read_tree(n);}
