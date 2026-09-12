/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
static int out(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);vprintf(fmt,a);va_end(a);return 0;}
int main(void){return out("%d",1);}
