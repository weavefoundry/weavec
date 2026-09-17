#include "api.h"
int main(void){static const unsigned char input[]="abc";struct reader r={input,sizeof input,0};struct node *p=make();if(!p)return 0;build(p,&r);drop(p);return 0;}
