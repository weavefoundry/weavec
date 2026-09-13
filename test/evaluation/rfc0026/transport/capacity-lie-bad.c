#include "runtime.h"
int main(void) { struct buffer b={0}; if(append(&b,7))return 0; b.capacity=100; b.length=99; if(append(&b,8)){destroy(&b);return 0;} destroy(&b);return 0; }
