#include "tree-api.h"
int main(void) {struct node *p=make(2);if(!p)return 0;struct node *saved=p->left;destroy(p);return saved?saved->value:0;}
