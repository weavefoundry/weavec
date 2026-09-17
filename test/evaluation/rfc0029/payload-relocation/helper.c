#include "api.h"
#include <stdlib.h>
int main(void){struct Node*n=calloc(1,sizeof *n);if(!n)return 0;fill(n);n->name=n->text;n->text=0;drop(n);return 0;}
