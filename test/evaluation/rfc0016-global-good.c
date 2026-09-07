// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
char*shared;static void zap(char*a){*shared=1;free(a);}
void run(void) { char *p = malloc(4); if (!p) return; shared=p;zap(p); }
