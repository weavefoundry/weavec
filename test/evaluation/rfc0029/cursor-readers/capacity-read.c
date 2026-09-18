#include "reader.h"
int main(void){unsigned char data[2]={1,2};struct reader r={0,data,3,2};return take(&r);}
