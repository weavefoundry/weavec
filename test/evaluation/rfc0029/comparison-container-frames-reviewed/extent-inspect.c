#include "api.h"
void unknown(struct node *);
unsigned inspect(struct node *n,const char *s){if(memcmp(s,"okay",4)==0)return read_tree(n);return read_tree(n);}
