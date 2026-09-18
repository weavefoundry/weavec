#include "api.h"
int main(void) { const unsigned char data[]={1,2,3}; struct node *p=0; if(!build(data,4,&p))return 0; unsigned value=sum(p); destroy(p); return value!=6; }
