#include "tree-api.h"
static void partial(struct node *p){if(!p)return;partial(p->left);free(p);}
int main(void){struct node *p=make(1);if(!p)return 0;p->right=make(1);partial(p);return 0;}
