#include <stdlib.h>
#include <limits.h>
int main(void){char text[]="123";text[0]='n';text[1]='a';text[2]='n';double x=strtod(text,0);if(x >= INT_MAX)return INT_MAX;else if(x <= (double)INT_MIN)return INT_MIN;else return (int)x;}
