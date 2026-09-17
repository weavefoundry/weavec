#include "api.h"
#include <stdlib.h>
int fill(struct Node*n,unsigned*cursor){*cursor=1;char*p=malloc(4);if(!p)return 0;p[0]=0;n->text=p;return 1;}
