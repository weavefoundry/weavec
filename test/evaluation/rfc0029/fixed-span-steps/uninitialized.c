#include "api.h"
int main(void){unsigned char src[10];src[0]=0;const unsigned char *p=src,*end=src+10;while(p<end){unsigned n=consume(p,end);if(!n)break;p+=n;}return 0;}
