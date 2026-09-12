/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){double *p=malloc(sizeof *p);if(!p)return 0;*p=1;free(p);return fabs(*p)>0;}
