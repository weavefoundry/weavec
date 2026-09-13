#include "runtime.h"
int main(void) { struct buffer b={0}; if(reserve(&b,64))return 0; b.length=64; int result=b.data[b.length-1]; destroy(&b);return result; }
