#include "reader.h"
int main(void){const unsigned char input[]="1.0";struct reader r={input,0,sizeof input,0};return (int)scan(&r);}
