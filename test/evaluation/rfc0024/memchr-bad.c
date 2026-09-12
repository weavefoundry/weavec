/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){char *a=malloc(4);if(!a)return 0;memcpy(a,"abc",4);char *p=memchr(a,'b',3);free(a);return p ? *p : 0;}
