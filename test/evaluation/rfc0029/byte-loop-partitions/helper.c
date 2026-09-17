#include "api.h"
static unsigned inner(const unsigned char*p,unsigned n){unsigned i=0;while(i<n&&p[i]!=34)i++;return i;}
unsigned scan(const unsigned char*p,unsigned n){return inner(p,n);}
