#include "tree.h"
int main(void) {struct node *p=make(1);if(!p)return 0;struct node *child=make(3);attach(p,child);unsigned n=total(p);destroy(p);return n==42;}
