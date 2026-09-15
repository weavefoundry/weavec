#include "tree.h"
int main(void) {struct node *p=make(1);if(!p)return 0;attach(p,p);destroy(p);return 0;}
