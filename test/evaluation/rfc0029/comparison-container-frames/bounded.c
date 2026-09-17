#include "api.h"
int main(void){struct node *n=calloc(1,sizeof *n);if(!n)return 0;n->value=3;const char s[]="ok";int r=inspect(n,s);drop(n);return r;}
