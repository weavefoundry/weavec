#include <stdlib.h>
#include <limits.h>
int main(void){const char text[]="12.5e-2";double x=strtod(text,0);if(x >= INT_MAX)return INT_MAX;else if(x <= (double)INT_MIN)return INT_MIN;else return (int)x;}
