#include "api.h"
void unknown(struct node *);
unsigned inspect(struct node *n,const char *s){char *end=0;struct {char *end;} local={0};(void)strtod(s,&n->text);return read_tree(n);}
