#include "reader.h"
int main(void) { unsigned char input[5]={'x','0','1','0','x'}; struct reader r={input,5,1,0}; return scan(&r); }
