#include "runtime.h"
int main(void){char b[32];int r=render(b,sizeof b,"%s:%d","ok",42);if(r<0)return 0;return strlen(b);}
