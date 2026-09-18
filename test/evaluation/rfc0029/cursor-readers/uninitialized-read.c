#include "reader.h"
int main(void){unsigned char data[3];data[0]=1;struct reader r={0,data,3,1};return take(&r);}
