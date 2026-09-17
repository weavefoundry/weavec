#include "api.h"
int main(void){struct node *n=calloc(1,sizeof *n);if(!n)return 0;n->value=3;const char s[]="12";unsigned r=inspect(n,s);return r==3?0:1;}
