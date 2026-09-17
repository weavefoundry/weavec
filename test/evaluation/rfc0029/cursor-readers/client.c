#include "reader.h"
int main(void){struct reader r={0,(const unsigned char*)"abc",3,0};take(&r);return r.pos<r.limit?r.data[r.pos]:0;}
