#include <stdlib.h>
#include "node.h"
int main(int argc, char **argv) { (void)argv; struct node *a=build((unsigned)argc), *b=a; destroy(a); unsigned n=count(b); return n>100; }
