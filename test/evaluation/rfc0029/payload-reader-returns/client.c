#include "api.h"
#include <stdlib.h>
int main(void){struct Node*n=calloc(1,sizeof *n);if(!n)return 0;static const unsigned char bytes[]="abc";struct Reader r={bytes,sizeof bytes,0,0};wrap(n,&r);drop(n);return 0;}
