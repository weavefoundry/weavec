/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){char b[128];return snprintf(b,128,"%ld %zu %.2f %p %%",1L,(size_t)2,3.0,(void*)b)<0;}
