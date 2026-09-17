#include "api.h"
int main(void) {const unsigned char data[]={1,2,3};struct node *p=build(data,4);unsigned n=sum(p);destroy(p);return n != 6;}
