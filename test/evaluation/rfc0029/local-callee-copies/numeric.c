#include <stdlib.h>
#include <string.h>
#include <stddef.h>
int main(void){char *p=malloc(3);if(!p)return 0;memcpy(p,"12",3);char *end=0;(void)strtod(p,&end);ptrdiff_t n=end-p;free(p);return (int)n;}
