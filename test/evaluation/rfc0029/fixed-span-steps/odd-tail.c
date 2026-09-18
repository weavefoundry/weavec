#include "api.h"
int main(void){const unsigned char src[9]={0};const unsigned char *p=src,*end=src+9;while(p<end){unsigned n=consume(p,end);if(!n)break;p+=n;}return 0;}
