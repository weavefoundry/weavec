#include "api.h"
int main(void){const unsigned char src[10]={0};const unsigned char *p=src,*end=src+10;while(p<end){unsigned n=consume(p,end);if(!n)break;p+=n+1;}return 0;}
