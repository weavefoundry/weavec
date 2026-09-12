#include "runtime.h"
int main(void){char b[32];int (*f)(char *,size_t,const char *,...)=render;int r=f(b,sizeof b,"%s","ok");if(r<0)return 0;return strlen(b);}
