#include "api.h"
#include <stdlib.h>
int fill(struct Node*n,unsigned*cursor){char*p=malloc(4);if(!p)goto fail;p[0]=0;n->text=p;n->name=p;*cursor=1;return 1;fail:*cursor=1;return 0;}
