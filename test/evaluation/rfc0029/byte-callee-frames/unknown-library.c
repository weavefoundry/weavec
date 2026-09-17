#include "api.h"
void unknown(struct state*);
int inspect(struct state*s,const unsigned char*p){unknown(s);if(p[0]!=97)return p[8];return 0;}
