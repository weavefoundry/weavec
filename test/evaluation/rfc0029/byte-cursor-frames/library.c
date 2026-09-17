#include "api.h"
#include <stdlib.h>
int inspect(const unsigned char *p,unsigned n) {
  unsigned char *out=malloc(n); if(!out)return 0;
  unsigned char *q=out; unsigned char **slot=&q;
  for(unsigned i=0;i<n;++i) {
    if(p[i]=='\\') {free(out);return p[n];}
    *(*slot)++=p[i];
  }
  free(out);return 0;
}
