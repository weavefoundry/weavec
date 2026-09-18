#include "api.h"
char *duplicate(const char *s){if(!s)return 0;size_t n=strlen(s)+1;char *p=malloc(n);if(!p)return 0; memcpy(p,s,n);return p;}
