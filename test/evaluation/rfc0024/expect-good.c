/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(int n,char **v){(void)v;char a[4]={0};if(__builtin_expect(n>=0 && n<4,1))return a[n];return 0;}
