#include <stdlib.h>
#include "node.h"
int main(int argc, char **argv) { (void)argv; struct node *a=build((unsigned)argc); struct node *p=concat(a,a); unsigned n=count(p); destroy(p); return n>100; }
