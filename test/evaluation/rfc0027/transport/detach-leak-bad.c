#include "tree-api.h"
int main(void) {struct node *p=make(3);if(!p)return 0;(void)detach(p);destroy(p);return 0;}
