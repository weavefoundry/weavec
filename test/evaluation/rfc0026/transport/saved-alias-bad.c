#include "runtime.h"
int main(void) { struct buffer b={0}; if(append(&b,7))return 0; unsigned char *old=b.data; if(reserve(&b,128)){destroy(&b);return 0;} int result=*old; destroy(&b);return result; }
