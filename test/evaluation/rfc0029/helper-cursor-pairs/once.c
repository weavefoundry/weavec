#include "api.h"
int main(void){const unsigned char bytes[10]={0};unsigned char dst[10];const unsigned char *p=bytes,*end=bytes+10;unsigned char *out=dst;decode(p,end,&out);return 0;}
