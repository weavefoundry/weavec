#include "buffer.h"
int main(void) { struct buffer b={0}; if(append(&b,7))return 0; b.data=0; return 0; }
