#include <stdlib.h>
#include "node.h"
int main(int argc, char **argv) { (void)argv; struct node *p=build((unsigned)argc); unsigned n=count(p); destroy(p); return n>100; }
