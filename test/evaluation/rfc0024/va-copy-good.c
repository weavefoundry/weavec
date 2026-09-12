/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
static int out(const char *fmt,...){va_list a,b;va_start(a,fmt);va_copy(b,a);vprintf(fmt,a);vprintf(fmt,b);va_end(a);va_end(b);return 0;}
int main(void){return out("%d",1);}
