#include "tree-api.h"
int main(void) {struct node *p=make(1);if(!p)return 0;struct node *child=make(1);p->left=child;p->right=child;destroy(p);return 0;}
