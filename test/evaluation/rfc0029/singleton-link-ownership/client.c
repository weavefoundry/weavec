#include "api.h"
int main(void){struct node *p=make();if(!p)return 0;struct reader r={0,2};build(p,&r);drop(p);return 0;}
