#include "api.h"
#include <stdlib.h>
int fill(struct Node *n){char *p=malloc(4);if(!p)return 0;p[0]=0;n->text=p+1;return 1;}
