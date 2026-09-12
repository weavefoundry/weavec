#include "runtime.h"
int render(char *b,size_t n,const char *fmt,...){va_list a;va_start(a,fmt);int r=vsnprintf(b,n,fmt,a);va_end(a);return r;}
