#include "api.h"
static unsigned config;
static void change(struct state*s){s->n=1;config=1;}
int inspect(struct state*s,const unsigned char*p){change(s);if(p[0]!=97)return p[8];return 0;}
