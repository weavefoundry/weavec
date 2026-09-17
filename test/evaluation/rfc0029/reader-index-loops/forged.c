#include "reader.h"
int main(void) { unsigned char input[2]={'x','0'}; struct reader r={input,5,1,0}; return scan(&r); }
