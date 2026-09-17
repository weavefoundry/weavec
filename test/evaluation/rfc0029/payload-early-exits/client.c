#include "api.h"
#include <stdlib.h>
int main(void){struct Node*n=calloc(1,sizeof *n);if(!n)return 0;unsigned cursor=0;fill(n,&cursor);drop(n);return 0;}
