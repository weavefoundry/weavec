#include <stdlib.h>
#include "node.h"
int main(int argc, char **argv) { (void)argv; struct node *p=build((unsigned)argc), *q=pop(&p); destroy(p); if(q) { free(q); return q->value; } return 0; }
