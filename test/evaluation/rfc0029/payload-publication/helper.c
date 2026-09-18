#include "api.h"
#include <stdlib.h>
static char *make(void){char *p=malloc(4);if(p)p[0]=0;return p;}
int fill(struct Node *n){char *p=make();if(!p)return 0;n->text=p;return 1;}
