#define ELEMENT unsigned char *
#include "buffer.h"
int main(void){struct buffer b={0};unsigned char *p=malloc(1);if(!p)return 0;
if(append(&b,p)){free(p);return 0;}if(append(&b,p)){free(p);destroy(&b);return 0;}
free(b.data[0]);free(b.data[1]);destroy(&b);return 0;}
