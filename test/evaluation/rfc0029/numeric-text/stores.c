#include <stdlib.h>
#include <limits.h>
int convert(unsigned digit){if(digit>9)return 0;char text[2];text[0]=(char)('0'+digit);text[1]=0;double x=strtod(text,0);if(x >= INT_MAX)return INT_MAX;else if(x <= (double)INT_MIN)return INT_MIN;else return (int)x;}
int main(void){return convert(3);}
