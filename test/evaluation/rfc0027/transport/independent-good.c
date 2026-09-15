#include "tree-api.h"
int main(void) {struct node *a=make(2),*b=make(3);destroy(a);unsigned n=total(b);destroy(b);return n==42;}
