#include "reader.h"
int main(void){unsigned char data[3]={1,2,3};struct reader r={0,data,3,4};return take(&r);}
