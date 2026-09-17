#include "api.h"
int main(void) {unsigned char data[3];data[0]=1;struct node *p=build(data,3);unsigned n=sum(p);destroy(p);return n != 6;}
