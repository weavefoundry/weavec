#include <stdlib.h>
#include "node.h"
int main(int argc, char **argv) { (void)argv; struct node *a=build((unsigned)argc), *b=build(2); struct node *p=concat(a,b); unsigned n=count(p); destroy(p); return n>100; }
