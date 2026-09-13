#define ELEMENT unsigned int
#include "buffer.h"
int main(void) { struct buffer b={0}; if(reserve(&b,SIZE_MAX))return 0; destroy(&b);return 0; }
