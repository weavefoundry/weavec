#include "buffer.h"
int main(void) { unsigned char bytes[16]={0}; struct buffer b={bytes,1,16}; destroy(&b);return 0; }
