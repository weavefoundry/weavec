/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){char b[8],s[3]={1,2,3};return snprintf(b,8,"%.*s",4,s)<0;}
