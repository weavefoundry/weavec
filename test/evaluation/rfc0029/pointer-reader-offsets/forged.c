#include "reader.h"
int main(void) { const unsigned char data[4]={'a','b','c','x'}; struct reader r={data,40,0,0};return scan(&r); }
