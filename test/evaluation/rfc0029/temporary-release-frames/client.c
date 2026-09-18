#include "api.h"
int main(void){struct node *n=calloc(1,sizeof *n);if(!n)return 0;n->value=3;unsigned r=inspect(n);drop(n);return r==3?0:1;}
