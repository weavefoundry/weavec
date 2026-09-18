#include "reader.h"
int main(void) { unsigned char input[5]; input[0]='x'; struct reader r={input,5,1,0}; return scan(&r); }
