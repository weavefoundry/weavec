/* RFC 0024 frozen runtime contract case. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdarg.h>
static int out(char *b,size_t n,const char *fmt,...){va_list ap;va_start(ap,fmt);int r=vsnprintf(b,n,fmt,ap);va_end(ap);return r;}
int main(void){char b[16];return out(b,16,"%s",7)<0;}
