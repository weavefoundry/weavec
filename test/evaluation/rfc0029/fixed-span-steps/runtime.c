#include "api.h"
int main(int argc,char **argv){(void)argv;unsigned length=(unsigned)argc;if(length>10)return 0;const unsigned char src[10]={0};const unsigned char *p=src,*end=src+length;while(p<end){unsigned n=consume(p,end);if(!n)break;p+=n;}return 0;}
