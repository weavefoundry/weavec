#include "runtime.h"
extern int choose;
static int unknown(char*b,size_t n,const char*fmt,...){return *(int*)0;}
int main(void){char b[32];int (*f)(char *,size_t,const char *,...)=choose?render:unknown;return f(b,sizeof b,"%s","ok");}
