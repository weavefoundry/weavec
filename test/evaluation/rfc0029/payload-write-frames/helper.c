#include "api.h"
#include <stdlib.h>
static void step(unsigned*p){*p=1;}
int fill(struct Node*n,unsigned*cursor){char*p=malloc(4);if(!p){step(cursor);return 0;}p[0]=0;n->text=p;step(cursor);return 1;}
