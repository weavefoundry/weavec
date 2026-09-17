#include "api.h"
int main(void){struct node *n=calloc(1,sizeof *n);if(!n)return 0;n->value=3;char s[3];s[0]='o';int r=inspect(n,s);drop(n);return r;}
