#include "api.h"
int main(void){const unsigned char bytes[10]={0};unsigned char dst[1];const unsigned char *p=bytes,*end=bytes+10;unsigned char *out=dst;while(p<end){unsigned n=decode(p,end,&out);if(!n)break;p+=n;}return 0;}
