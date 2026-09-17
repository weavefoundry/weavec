#include "api.h"
size_t scan(const unsigned char *p,size_t n,size_t start){size_t i;for(i=0;start+i<n;i++){switch(p[start+i]){case '1':case '2':break;default:goto done;}}done:return i;}
