#include "tree.h"
int main(void) {struct node *p=malloc(sizeof *p);if(!p)return 0;*p=(struct node){1,0,0};destroy(p);return 0;}
