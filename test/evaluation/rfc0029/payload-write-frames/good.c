#include "api.h"
#include <stdlib.h>
int fill(struct Node*n,unsigned*cursor){char*p=malloc(4);if(!p){*cursor=1;return 0;}p[0]=0;n->text=p;*cursor=1;return 1;}
