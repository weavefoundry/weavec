#include "runtime.h"
static int logmsg(const char *fmt,...){va_list a;va_start(a,fmt);int r=forward(fmt,a);va_end(a);return r;}
int main(void){return logmsg("%s", "ok");}
