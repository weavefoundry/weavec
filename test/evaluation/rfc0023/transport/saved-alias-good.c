#include <stdlib.h>
#include "node.h"
int main(int argc, char **argv) { (void)argv; struct node *p=build((unsigned)argc), *q=p; unsigned n=count(q); destroy(p); return n>100; }
