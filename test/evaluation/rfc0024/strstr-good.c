/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
int main(void){char a[4]="abc";char *p=strstr(a,"bc");return p ? *p : 0;}
