#include "api.h"
static char *make(void){return malloc(4);}
unsigned inspect(struct node *n){char *p=malloc(4);if(!p)return read_tree(n);free(p+1);return read_tree(n);}
