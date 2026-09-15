#include "tree-api.h"
int main(void) {struct node *p=make(3);if(!p)return 0;struct node *child=detach(p);destroy(p);unsigned n=total(child);destroy(child);return n==42;}
