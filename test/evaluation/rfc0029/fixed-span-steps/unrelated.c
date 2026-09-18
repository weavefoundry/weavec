#include "api.h"
int main(void){const unsigned char src[10]={0};const unsigned char other[10]={0};const unsigned char *p=src,*end=other+10;while(p<end){unsigned n=consume(p,end);if(!n)break;p+=n;}return 0;}
