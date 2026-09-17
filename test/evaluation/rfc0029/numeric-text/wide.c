#include <stdlib.h>
#include <limits.h>
long long convert(void){const char text[]="9223372036854775808";double x=strtod(text,0);if(x>9223372036854775808.0)return 0;else if(x < -9223372036854775808.0)return 0;else return (long long)x;}
int main(void){return convert()!=0;}
