#include <stdlib.h>
#include <string.h>
#include <stddef.h>
int main(void){unsigned char *p=malloc(3);if(!p)return 0;memcpy(p,"12",3);unsigned char *end=0;(void)strtod((const char*)p,(char**)&end);ptrdiff_t n=end-p;free(p);return (int)n;}
