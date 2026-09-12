#include "runtime.h"
int forward(const char *fmt,va_list a){return vprintf(fmt,a);}
