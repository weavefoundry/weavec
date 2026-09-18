#include "api.h"
unsigned scan(const unsigned char*p,unsigned n){unsigned i=0;if(!n)return 0;do{if(p[i]==34)return i;i++;}while(i<n);return i;}
