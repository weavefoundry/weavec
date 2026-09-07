// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
static void drop(char *p){free(p);}
static void zap(void(*fn)(char*),char*a,char*b){fn(a);*b=1;} // BUG: callback
void run(void) { char *p = malloc(4); if (!p) return; zap(drop,p,p); }
