/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
static void f(const char *s,...){va_list a;va_start(a,s);va_end(a);vprintf(s,a);}int main(void){f("%d",1);return 0;}
