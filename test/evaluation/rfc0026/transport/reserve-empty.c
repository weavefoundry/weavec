#include "runtime.h"
int main(void) { struct buffer b={0}; if(reserve(&b,64))return 0; b.data[63]=7; int result=b.data[63]; destroy(&b); return result; }
