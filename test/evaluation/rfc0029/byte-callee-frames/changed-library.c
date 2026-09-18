#include "api.h"
static void change(struct state*s,unsigned char*p){s->n=1;p[0]=98;}
int inspect(struct state*s,const unsigned char*p){change(s,(unsigned char*)p);if(p[0]!=97)return p[8];return 0;}
