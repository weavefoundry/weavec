#include "api.h"
int inspect(struct node *n){void *p=malloc(4);if(!p)return 0;free(p);free(n);return 1;}
