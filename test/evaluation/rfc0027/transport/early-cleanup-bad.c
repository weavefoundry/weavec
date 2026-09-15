#include "tree-api.h"
static void partial(struct node *p){if(!p)return;if(p->value)return;partial(p->left);partial(p->right);free(p);}
int main(void){struct node *p=make(2);partial(p);return 0;}
