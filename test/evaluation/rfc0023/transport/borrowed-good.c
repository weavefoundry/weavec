#include <stdlib.h>
#include "node.h"
int main(void) { struct node b={2,NULL}, a={1,&b}; return count(&a)!=2; }
