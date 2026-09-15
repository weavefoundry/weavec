#include "tree-api.h"
int main(void) {struct node *p=make(2);if(!p)return 0;struct node *right=make(3);p->right=right;unsigned n=total(p);destroy(p);return n==42;}
