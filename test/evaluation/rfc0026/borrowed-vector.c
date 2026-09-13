#define ELEMENT unsigned char *
#include "buffer.h"
int main(void){struct buffer b={0};unsigned char v=7;
if(append(&b,&v))return 0;int result=*b.data[0];destroy(&b);return result;}
