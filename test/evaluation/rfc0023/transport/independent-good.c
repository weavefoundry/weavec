#include <stdlib.h>
#include "node.h"
int main(int argc, char **argv) { (void)argv; struct node *a=build((unsigned)argc), *b=build(2); destroy(a); unsigned n=count(b); destroy(b); return n>100; }
