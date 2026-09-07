// RFC 0016: paired compositional call evaluation.
#include "Inputs/heap.h"
void zap(char*,char*);
void run(void) { char *p = malloc(4); if (!p) return; zap(p,p);}
