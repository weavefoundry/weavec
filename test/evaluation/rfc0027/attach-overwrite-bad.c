#include "tree.h"
int main(void) {struct node *p=make(2);if(!p)return 0;struct node *child=make(3);attach(p,child);destroy(p);return 0;}
