#include "reader.h"
int main(void){struct reader original={0,(const unsigned char*)"abc",3,0};struct reader r=original;take(&r);return r.pos<r.limit?r.data[r.pos]:0;}
