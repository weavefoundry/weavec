#include "api.h"
static char *copy(const char *s,struct hooks *h){if(!s)return 0;size_t n=strlen(s)+1;char *p=h->allocate(n);if(!p)return 0; memcpy(p,s,n);return p;}
char *duplicate(const char *s){struct hooks h={malloc};return copy(s,&h);}
