#include <stdlib.h>
#include <limits.h>
int main(void){char text[4];text[0]='1';text[3]=0;double x=strtod(text,0);if(x >= INT_MAX)return INT_MAX;else if(x <= (double)INT_MIN)return INT_MIN;else return (int)x;}
